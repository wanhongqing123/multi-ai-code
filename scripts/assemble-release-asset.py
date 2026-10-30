#!/usr/bin/env python3
"""在 GitHub runner 合并草稿 Release 的分段资产，并校验后上传完整文件。"""
import hashlib
import json
import os
import re
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def gh(*args, output=None):
    # 认证只从 runner 的 GH_TOKEN 读取，不将令牌放进参数或输出。
    result = subprocess.run(
        ['gh', *args], stdout=output if output is not None else subprocess.PIPE,
        stderr=subprocess.PIPE, check=False, timeout=600,
    )
    if result.returncode:
        raise RuntimeError('GitHub operation failed: ' + args[0])
    return result.stdout


def settings():
    repo = os.environ['GITHUB_REPOSITORY']
    tag = os.environ['RELEASE_TAG']
    name = os.environ['ASSET_NAME']
    count = int(os.environ['PART_COUNT'])
    size = int(os.environ['ASSET_SIZE'])
    digest = os.environ['ASSET_SHA256'].lower()
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo):
        raise ValueError('Invalid repository')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._/-]{0,199}', tag):
        raise ValueError('Invalid tag')
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,199}', name) or '..' in name:
        raise ValueError('Invalid asset name')
    if not 1 <= count <= 9999 or not 1 <= size <= 5 * 1024**3:
        raise ValueError('Invalid asset bounds')
    if not re.fullmatch(r'[0-9a-f]{64}', digest):
        raise ValueError('Invalid SHA256')
    return repo, tag, name, count, size, digest


def file_digest(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def draft(repo, tag):
    release = json.loads(gh('api', f'repos/{repo}/releases/tags/{tag}'))
    if not release.get('draft'):
        raise ValueError('Only draft releases can be assembled')
    return release


def matches(asset, size, digest):
    return (asset.get('state') == 'uploaded' and asset['size'] == size
            and asset.get('digest') == 'sha256:' + digest)


def main():
    repo, tag, name, count, size, digest = settings()
    release = draft(repo, tag)
    complete = next((a for a in release['assets'] if a['name'] == name), None)
    if complete:
        if matches(complete, size, digest):
            print('Complete asset already matches')
            return
        raise ValueError('An existing complete asset has different content')
    expected = [f'{name}.part{i:04d}' for i in range(count)]
    parts = {a['name']: a for a in release['assets'] if a['name'].startswith(name + '.part')}
    if set(parts) != set(expected):
        raise ValueError('Part names or count do not match')
    if sum(a['size'] for a in parts.values()) != size:
        raise ValueError('Part sizes do not match')
    if any(a.get('state') != 'uploaded' for a in parts.values()):
        raise ValueError('A part is incomplete')

    with tempfile.TemporaryDirectory(prefix='release-assembly-') as directory:
        root = Path(directory)

        def download(part_name):
            asset = parts[part_name]
            path = root / part_name
            with path.open('wb') as output:
                gh('api', '-H', 'Accept: application/octet-stream',
                   f"repos/{repo}/releases/assets/{asset['id']}", output=output)
            if (path.stat().st_size != asset['size']
                    or asset.get('digest') != 'sha256:' + file_digest(path)):
                raise ValueError('Downloaded part failed integrity check')

        with ThreadPoolExecutor(max_workers=8) as executor:
            list(executor.map(download, expected))
        result = root / name
        with result.open('wb') as output:
            for part_name in expected:
                with (root / part_name).open('rb') as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b''):
                        output.write(block)
        if result.stat().st_size != size or file_digest(result) != digest:
            raise ValueError('Assembled file differs from the verified build')
        # 下载期间 Release 可能被其他操作者发布；上传前再次校验草稿状态。
        current = draft(repo, tag)
        if current['id'] != release['id']:
            raise ValueError('Release identity changed')
        gh('release', 'upload', tag, str(result), '--repo', repo)
        current = draft(repo, tag)
        complete = next((a for a in current['assets'] if a['name'] == name), None)
        if not complete or not matches(complete, size, digest):
            raise ValueError('Uploaded complete asset failed integrity check')
    print(f'Assembled and uploaded {name}: {size} bytes, SHA256 {digest}')


if __name__ == '__main__':
    main()
