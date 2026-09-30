package com.kongshang.maichat;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;

/** Reads complete AAC/ADTS frames from a recording that is still growing. */
final class AdtsFrameReader implements AutoCloseable {
    private final RandomAccessFile file;
    private long offset, samples;
    AdtsFrameReader(File source) throws IOException { file = new RandomAccessFile(source, "r"); }
    byte[] readUntilSample(long sampleBudget) throws IOException {
        ByteArrayOutputStream packet = new ByteArrayOutputStream();
        byte[] header = new byte[7];
        while (file.length() - offset >= 7) {
            file.seek(offset); file.readFully(header);
            if ((header[0] & 255) != 255 || (header[1] & 0xf6) != 0xf0) throw new IOException("Invalid AAC frame");
            int size = ((header[3] & 3) << 11) | ((header[4] & 255) << 3) | ((header[5] & 0xe0) >> 5);
            int headerSize = (header[1] & 1) == 0 ? 9 : 7;
            if (size < headerSize) throw new IOException("Invalid AAC frame size");
            int frameSamples = ((header[6] & 3) + 1) * 1024;
            if (samples + frameSamples > sampleBudget || file.length() - offset < size) break;
            byte[] frame = new byte[size]; file.seek(offset); file.readFully(frame);
            packet.write(frame); offset += size; samples += frameSamples;
        }
        return packet.toByteArray();
    }
    boolean atEnd() throws IOException { return file.length() == offset; }
    long samples() { return samples; }
    @Override public void close() throws IOException { file.close(); }
}
