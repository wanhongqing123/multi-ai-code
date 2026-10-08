'use strict';

const http = require('node:http');
const { handler } = require('./index');

const MAX_REQUEST_BYTES = 4096;

function createServer(handle = handler) {
  return http.createServer(async (request, response) => {
    const chunks = [];
    let totalBytes = 0;
    try {
      for await (const chunk of request) {
        totalBytes += chunk.length;
        if (totalBytes > MAX_REQUEST_BYTES) {
          response.writeHead(413, { 'Content-Type': 'application/json',
            'Cache-Control': 'no-store' });
          response.end(JSON.stringify({ error: 'Request is too large' }));
          return;
        }
        chunks.push(chunk);
      }
      const body = Buffer.concat(chunks, totalBytes).toString('utf8');
      const result = await handle({
        httpMethod: request.method,
        path: new URL(request.url, 'http://localhost').pathname,
        headers: request.headers,
        body
      });
      response.writeHead(result.statusCode, result.headers);
      response.end(result.body);
    } catch {
      if (!response.headersSent) {
        response.writeHead(500, { 'Content-Type': 'application/json',
          'Cache-Control': 'no-store' });
      }
      response.end(JSON.stringify({ error: 'Internal service error' }));
    }
  });
}

if (require.main === module) {
  const port = Number(process.env.MAICHAT_MEDIA_PORT || 9000);
  if (!Number.isInteger(port) || port < 1 || port > 65535)
    throw new Error('MAICHAT_MEDIA_PORT must be a TCP port');
  createServer().listen(port, '127.0.0.1');
}

exports.createServer = createServer;
