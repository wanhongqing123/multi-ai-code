# 文件作用：为移动端适配层测试提供本机模型接口响应，不调用付费模型。
# 阅读边界：此文件负责构建或测试流程，不在应用运行时解释为业务逻辑。
"""Local-only deterministic model fixture for iOS/Android UI tests; never a product server."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import time

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        prompt = body['messages'][-1].get('content', '')
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        try:
            if prompt == 'write':
                call = {'index': 0, 'id': 'call_mobile_write', 'type': 'function', 'function': {
                    'name': 'file_write',
                    'arguments': json.dumps({'path': 'ui-result.txt', 'content': 'approved'})}}
                response = {'choices': [{'index': 0, 'delta': {'tool_calls': [call]},
                                         'finish_reason': 'tool_calls'}]}
                self.wfile.write(('data: ' + json.dumps(response) + '\n\ndata: [DONE]\n\n').encode())
                self.wfile.flush()
                return
            if prompt == 'media cards':
                calls = [{'index': 0, 'id': 'call_resources', 'type': 'function',
                          'function': {'name': 'system_resources', 'arguments': '{}'}}]
                for index, name in enumerate(('parity-one.mp4', 'parity-two.mp4'), start=1):
                    calls.append({'index': index, 'id': 'call_media_' + str(index),
                                  'type': 'function', 'function': {
                                      'name': 'agent_send_media',
                                      'arguments': json.dumps({'file_path': name, 'type': 'video'})}})
                response = {'choices': [{'index': 0, 'delta': {'tool_calls': calls},
                                         'finish_reason': 'tool_calls'}]}
                self.wfile.write(('data: ' + json.dumps(response) + '\n\ndata: [DONE]\n\n').encode())
                self.wfile.flush()
                return
            if prompt == 'audio card':
                call = {'index': 0, 'id': 'call_audio', 'type': 'function',
                        'function': {'name': 'agent_send_media', 'arguments': json.dumps({
                            'file_path': 'parity-audio.wav', 'type': 'audio'})}}
                response = {'choices': [{'index': 0, 'delta': {'tool_calls': [call]},
                                         'finish_reason': 'tool_calls'}]}
                self.wfile.write(('data: ' + json.dumps(response) + '\n\ndata: [DONE]\n\n').encode())
                self.wfile.flush()
                return
            answer = ('Long message paragraph.\n\n' * 45 + 'Tail marker'
                      if prompt == 'long' else 'Mobile response ready')
            deltas = [{'reasoning_content': 'Checking the mobile interface.'},
                      {'content': answer[:len(answer)//2]},
                      {'content': answer[len(answer)//2:]}]
            for delta in deltas:
                response = {'choices': [{'index': 0, 'delta': delta, 'finish_reason': None}]}
                self.wfile.write(('data: ' + json.dumps(response) + '\n\n').encode())
                self.wfile.flush()
                time.sleep(3 if prompt == 'stop' else .25)
            self.wfile.write(b'data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

if __name__ == '__main__':
    port = int(os.environ.get('MAICHAT_TEST_MODEL_PORT', '18189'))
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
