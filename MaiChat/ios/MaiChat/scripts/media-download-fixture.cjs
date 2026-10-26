'use strict';

// Simulator-only local TLS media server. The private key is generated at runtime
// and never shipped in the repository or the app.
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const http = require('node:http');
const https = require('node:https');
const { execFileSync } = require('node:child_process');

const tlsPort = 18442;
const certPort = 18441;
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'maichat-media-tls-'));
const certPath = path.join(directory, 'localhost.pem');
const keyPath = path.join(directory, 'localhost.key');
execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-sha256',
  '-nodes', '-keyout', keyPath, '-out', certPath, '-days', '1',
  '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'],
{ stdio: 'ignore' });
const cert = fs.readFileSync(certPath);
const key = fs.readFileSync(keyPath);
fs.rmSync(directory, { recursive: true, force: true });

const image = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l5sAAAAASUVORK5CYII=', 'base64');
const file = Buffer.from('MaiChat local TLS attachment fixture\n');
const tlsServer = https.createServer({ key, cert }, (request, response) => {
  if (request.url === '/image') {
    response.writeHead(200, { 'Content-Type': 'image/png', 'Content-Length': image.length });
    response.end(image);
  } else if (request.url === '/file') {
    response.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': file.length });
    response.end(file);
  } else if (request.url === '/held') {
    response.writeHead(200, { 'Content-Type': 'application/octet-stream' });
    response.write('started');
    const pending = setInterval(() => response.write(Buffer.alloc(1024)), 1000);
    response.on('close', () => clearInterval(pending));
  } else {
    response.writeHead(404);
    response.end();
  }
});
const certServer = http.createServer((request, response) => {
  if (request.url !== '/cert') { response.writeHead(404); response.end(); return; }
  response.writeHead(200, { 'Content-Type': 'application/x-pem-file' });
  response.end(cert);
});
tlsServer.listen(tlsPort, '127.0.0.1', () => {
  certServer.listen(certPort, '127.0.0.1', () => {
    process.stdout.write(`MaiChat media TLS fixture ${tlsPort}; cert ${certPort}\n`);
  });
});
