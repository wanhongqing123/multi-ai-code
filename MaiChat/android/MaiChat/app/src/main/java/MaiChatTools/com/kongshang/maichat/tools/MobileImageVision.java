package com.kongshang.maichat.tools;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.graphics.PointF;
import android.graphics.Rect;
import android.media.ExifInterface;
import com.google.android.gms.tasks.Tasks;
import com.google.mlkit.vision.common.InputImage;
import com.google.mlkit.vision.face.Face;
import com.google.mlkit.vision.face.FaceDetection;
import com.google.mlkit.vision.face.FaceDetector;
import com.google.mlkit.vision.face.FaceDetectorOptions;
import com.google.mlkit.vision.face.FaceLandmark;
import com.google.mlkit.vision.segmentation.Segmentation;
import com.google.mlkit.vision.segmentation.SegmentationMask;
import com.google.mlkit.vision.segmentation.Segmenter;
import com.google.mlkit.vision.segmentation.selfie.SelfieSegmenterOptions;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

/** On-device face localization and person masks for Agent workspace images. */
public final class MobileImageVision {
    private MobileImageVision() {}

    public static JSONObject detectFaces(File source) throws Exception {
        PreparedImage image = prepare(source);
        FaceDetectorOptions options = new FaceDetectorOptions.Builder()
            .setPerformanceMode(FaceDetectorOptions.PERFORMANCE_MODE_ACCURATE)
            .setLandmarkMode(FaceDetectorOptions.LANDMARK_MODE_ALL)
            .build();
        FaceDetector detector = FaceDetection.getClient(options);
        try {
            List<Face> detected = Tasks.await(
                detector.process(InputImage.fromBitmap(image.bitmap, 0)), 60, TimeUnit.SECONDS);
            double scaleX = (double) image.sourceWidth / image.bitmap.getWidth();
            double scaleY = (double) image.sourceHeight / image.bitmap.getHeight();
            JSONArray faces = new JSONArray();
            for (Face face : detected) {
                Rect bounds = face.getBoundingBox();
                JSONObject landmarks = new JSONObject();
                landmark(landmarks, "left_eye", face.getLandmark(FaceLandmark.LEFT_EYE), scaleX, scaleY);
                landmark(landmarks, "right_eye", face.getLandmark(FaceLandmark.RIGHT_EYE), scaleX, scaleY);
                landmark(landmarks, "mouth", face.getLandmark(FaceLandmark.MOUTH_BOTTOM), scaleX, scaleY);
                faces.put(new JSONObject()
                    .put("box", new JSONObject()
                        .put("x", Math.round(bounds.left * scaleX))
                        .put("y", Math.round(bounds.top * scaleY))
                        .put("width", Math.round(bounds.width() * scaleX))
                        .put("height", Math.round(bounds.height() * scaleY)))
                    .put("landmarks", landmarks)
                    .put("yaw_degrees", face.getHeadEulerAngleY())
                    .put("roll_degrees", face.getHeadEulerAngleZ()));
            }
            return new JSONObject().put("source_width", image.sourceWidth)
                .put("source_height", image.sourceHeight)
                .put("coordinate_origin", "top_left").put("faces", faces);
        } finally {
            detector.close();
            image.bitmap.recycle();
        }
    }

    public static JSONObject segmentPerson(File source, File target) throws Exception {
        PreparedImage image = prepare(source);
        SelfieSegmenterOptions options = new SelfieSegmenterOptions.Builder()
            .setDetectorMode(SelfieSegmenterOptions.SINGLE_IMAGE_MODE).build();
        Segmenter segmenter = Segmentation.getClient(options);
        Bitmap mask = null;
        try {
            SegmentationMask result = Tasks.await(
                segmenter.process(InputImage.fromBitmap(image.bitmap, 0)), 60, TimeUnit.SECONDS);
            int width = result.getWidth(), height = result.getHeight();
            if (width < 1 || height < 1 || (long) width * height > 12_000_000)
                throw new IllegalStateException("人物遮罩尺寸无效");
            ByteBuffer confidences = result.getBuffer();
            confidences.order(ByteOrder.nativeOrder());
            confidences.rewind();
            int[] pixels = new int[width * height];
            for (int index = 0; index < pixels.length; index++) {
                float confidence = confidences.getFloat();
                int value = Float.isFinite(confidence)
                    ? Math.round(Math.max(0, Math.min(1, confidence)) * 255) : 0;
                pixels[index] = 0xff000000 | value << 16 | value << 8 | value;
            }
            mask = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            mask.setPixels(pixels, 0, width, 0, 0, width, height);
            try (FileOutputStream output = new FileOutputStream(target)) {
                if (!mask.compress(Bitmap.CompressFormat.PNG, 100, output))
                    throw new IOException("无法写入人物遮罩");
            }
            if (target.length() < 1 || target.length() > 50L * 1024 * 1024)
                throw new IllegalStateException("人物遮罩文件大小无效");
            return new JSONObject().put("path", target.getName())
                .put("mime_type", "image/png").put("kind", "person_mask")
                .put("source_width", image.sourceWidth).put("source_height", image.sourceHeight)
                .put("mask_width", width).put("mask_height", height)
                .put("next_tool", "mobile_preview_image");
        } catch (Exception error) {
            target.delete();
            throw error;
        } finally {
            if (mask != null) mask.recycle();
            segmenter.close();
            image.bitmap.recycle();
        }
    }

    private static void landmark(JSONObject output, String name, FaceLandmark landmark,
                                 double scaleX, double scaleY) throws Exception {
        if (landmark == null) return;
        PointF point = landmark.getPosition();
        output.put(name, new JSONObject()
            .put("x", Math.round(point.x * scaleX))
            .put("y", Math.round(point.y * scaleY)));
    }

    private static PreparedImage prepare(File source) throws Exception {
        if (source.length() < 1 || source.length() > 50L * 1024 * 1024)
            throw new IllegalArgumentException("源图片不能超过 50 MB");
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(source.getPath(), bounds);
        if (bounds.outWidth < 1 || bounds.outHeight < 1
                || bounds.outWidth > 20_000 || bounds.outHeight > 20_000)
            throw new IllegalArgumentException("图片尺寸无效");
        int orientation = ExifInterface.ORIENTATION_NORMAL;
        try {
            orientation = new ExifInterface(source.getPath()).getAttributeInt(
                ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
        } catch (IOException ignored) { /* Images without EXIF are already upright. */ }
        boolean swapped = orientation == ExifInterface.ORIENTATION_ROTATE_90
            || orientation == ExifInterface.ORIENTATION_ROTATE_270
            || orientation == ExifInterface.ORIENTATION_TRANSPOSE
            || orientation == ExifInterface.ORIENTATION_TRANSVERSE;
        int sourceWidth = swapped ? bounds.outHeight : bounds.outWidth;
        int sourceHeight = swapped ? bounds.outWidth : bounds.outHeight;
        BitmapFactory.Options decode = new BitmapFactory.Options();
        decode.inPreferredConfig = Bitmap.Config.ARGB_8888;
        decode.inSampleSize = 1;
        while (Math.max(bounds.outWidth, bounds.outHeight) / decode.inSampleSize > 2048)
            decode.inSampleSize *= 2;
        Bitmap raw = BitmapFactory.decodeFile(source.getPath(), decode);
        if (raw == null) throw new IllegalArgumentException("无法解码源图片");
        Matrix matrix = new Matrix();
        switch (orientation) {
        case ExifInterface.ORIENTATION_FLIP_HORIZONTAL: matrix.setScale(-1, 1); break;
        case ExifInterface.ORIENTATION_ROTATE_180: matrix.setRotate(180); break;
        case ExifInterface.ORIENTATION_FLIP_VERTICAL: matrix.setScale(1, -1); break;
        case ExifInterface.ORIENTATION_TRANSPOSE:
            matrix.setRotate(90); matrix.postScale(-1, 1); break;
        case ExifInterface.ORIENTATION_ROTATE_90: matrix.setRotate(90); break;
        case ExifInterface.ORIENTATION_TRANSVERSE:
            matrix.setRotate(270); matrix.postScale(-1, 1); break;
        case ExifInterface.ORIENTATION_ROTATE_270: matrix.setRotate(270); break;
        default: break;
        }
        Bitmap upright = matrix.isIdentity() ? raw
            : Bitmap.createBitmap(raw, 0, 0, raw.getWidth(), raw.getHeight(), matrix, true);
        if (upright != raw) raw.recycle();
        if (Math.max(upright.getWidth(), upright.getHeight()) > 2048) {
            double scale = 2048.0 / Math.max(upright.getWidth(), upright.getHeight());
            Bitmap smaller = Bitmap.createScaledBitmap(upright,
                Math.max(1, (int) Math.round(upright.getWidth() * scale)),
                Math.max(1, (int) Math.round(upright.getHeight() * scale)), true);
            if (smaller != upright) upright.recycle();
            upright = smaller;
        }
        return new PreparedImage(upright, sourceWidth, sourceHeight);
    }

    private static final class PreparedImage {
        final Bitmap bitmap;
        final int sourceWidth, sourceHeight;
        PreparedImage(Bitmap bitmap, int sourceWidth, int sourceHeight) {
            this.bitmap = bitmap;
            this.sourceWidth = sourceWidth;
            this.sourceHeight = sourceHeight;
        }
    }
}
