package com.kongshang.maichat.tools;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.pdf.PdfDocument;
import android.text.Html;
import android.text.Spanned;
import android.text.StaticLayout;
import android.text.TextPaint;
import android.util.Base64;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

/** Generates a paginated A4 PDF using only Android's in-process drawing APIs. */
public final class AgentPdfRenderer {
    private static final int PAGE_WIDTH = 595;
    private static final int PAGE_HEIGHT = 842;
    private static final int MARGIN = 36;
    private static final int CONTENT_WIDTH = PAGE_WIDTH - 2 * MARGIN;
    private static final int CONTENT_HEIGHT = PAGE_HEIGHT - 2 * MARGIN;
    private static final int MAX_PAGES = 200;

    private AgentPdfRenderer() {}

    public static JSONObject render(String html, File output) throws Exception {
        if (html == null || html.isEmpty() || html.getBytes(StandardCharsets.UTF_8).length > 10 * 1024 * 1024)
            throw new IllegalArgumentException("HTML source is empty or exceeds 10 MB");
        Html.ImageGetter images = source -> embeddedImage(source);
        Spanned content = Html.fromHtml(html, Html.FROM_HTML_MODE_COMPACT, images, null);
        if (content.length() == 0)
            throw new IllegalArgumentException("HTML source has no printable content");
        TextPaint paint = new TextPaint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(android.graphics.Color.BLACK);
        paint.setTextSize(12);
        StaticLayout layout = StaticLayout.Builder.obtain(content, 0, content.length(), paint,
                CONTENT_WIDTH)
            .setIncludePad(false)
            .setLineSpacing(2, 1.15f)
            .build();
        PdfDocument document = new PdfDocument();
        int pageCount = 0;
        try {
            int firstLine = 0;
            do {
                if (++pageCount > MAX_PAGES)
                    throw new IllegalArgumentException("HTML document exceeds 200 PDF pages");
                int firstTop = layout.getLineTop(firstLine);
                int lastLine = firstLine;
                while (lastLine + 1 < layout.getLineCount()
                    && layout.getLineBottom(lastLine + 1) - firstTop <= CONTENT_HEIGHT)
                    lastLine++;
                PdfDocument.Page page = document.startPage(
                    new PdfDocument.PageInfo.Builder(PAGE_WIDTH, PAGE_HEIGHT, pageCount).create());
                Canvas canvas = page.getCanvas();
                canvas.drawColor(android.graphics.Color.WHITE);
                canvas.save();
                canvas.translate(MARGIN, MARGIN - firstTop);
                canvas.clipRect(0, firstTop, CONTENT_WIDTH,
                    firstTop + CONTENT_HEIGHT);
                layout.draw(canvas);
                canvas.restore();
                document.finishPage(page);
                firstLine = lastLine + 1;
            } while (firstLine < layout.getLineCount());
            if (!output.createNewFile())
                throw new IOException("PDF output already exists");
            try (FileOutputStream stream = new FileOutputStream(output)) {
                document.writeTo(stream);
            }
        } catch (Exception failure) {
            output.delete();
            throw failure;
        } finally {
            document.close();
        }
        if (!output.isFile() || output.length() < 8 || output.length() > 100L * 1024 * 1024)
            throw new IOException("PDF renderer did not produce a valid file");
        try (FileInputStream input = new FileInputStream(output)) {
            byte[] header = new byte[5];
            if (input.read(header) != header.length
                || !"%PDF-".equals(new String(header, StandardCharsets.US_ASCII)))
                throw new IOException("PDF output has an invalid header");
        }
        return new JSONObject().put("pages", pageCount).put("bytes", output.length());
    }

    private static Drawable embeddedImage(String source) {
        if (source == null || !source.startsWith("data:image/") || source.length() > 10 * 1024 * 1024)
            return null;
        int comma = source.indexOf(',');
        if (comma < 0 || !source.substring(0, comma).endsWith(";base64")) return null;
        try {
            byte[] bytes = Base64.decode(source.substring(comma + 1), Base64.DEFAULT);
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
            if (options.outWidth <= 0 || options.outHeight <= 0) return null;
            int sample = 1;
            while (options.outWidth / sample > 2048 || options.outHeight / sample > 2048)
                sample *= 2;
            options.inJustDecodeBounds = false;
            options.inSampleSize = sample;
            Bitmap bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
            if (bitmap == null) return null;
            BitmapDrawable image = new BitmapDrawable(null, bitmap);
            float scale = Math.min(1f, Math.min((float) CONTENT_WIDTH / bitmap.getWidth(),
                (float) CONTENT_HEIGHT / bitmap.getHeight()));
            int width = Math.max(1, Math.round(bitmap.getWidth() * scale));
            int height = Math.max(1, Math.round(bitmap.getHeight() * scale));
            image.setBounds(0, 0, width, height);
            return image;
        } catch (IllegalArgumentException failure) {
            return null;
        }
    }
}
