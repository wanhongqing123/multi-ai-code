// 文件作用：Android 联系人和群组管理；DefaultAvatarAtlas 负责默认头像资源的索引和绘制，本文件给出其实现。
// 主要入口：portrait。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

final class DefaultAvatarAtlas {
    private static Bitmap atlas;
    private static final Bitmap[] portraits = new Bitmap[26];

    private DefaultAvatarAtlas() {}

    static synchronized Bitmap portrait(Context context, int index) {
        if (portraits[index] != null) return portraits[index];
        if (atlas == null) {
            BitmapFactory.Options options = new BitmapFactory.Options();
            options.inScaled = false;
            atlas = BitmapFactory.decodeResource(context.getResources(),
                R.drawable.avatar_chibi_atlas_v3, options);
        }
        int tile = atlas.getWidth() / 6;
        Bitmap portrait = Bitmap.createBitmap(atlas,
            (index % 6) * tile, (index / 6) * tile, tile, tile);
        portraits[index] = portrait;
        return portrait;
    }
}
