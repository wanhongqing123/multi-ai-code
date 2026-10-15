// 文件作用：Android 设备集成测试；AgentPdfRendererInstrumentedTest 负责覆盖对应界面或服务的设备行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：generatesChinesePdfWithoutPrintDialog。
// 直接依赖：com.kongshang.maichat.tools.AgentPdfRenderer；实现与这些接口协作。
package com.kongshang.maichat;

import com.kongshang.maichat.tools.AgentPdfRenderer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class AgentPdfRendererInstrumentedTest {
    @Test public void generatesChinesePdfWithoutPrintDialog() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File output = new File(context.getCacheDir(), "agent-pdf-" + UUID.randomUUID() + ".pdf");
        try {
            StringBuilder html = new StringBuilder("<html><body><h1>中文报告</h1>");
            for (int i = 0; i < 180; i++)
                html.append("<p>第 ").append(i).append(" 段内容，用于测试分页。</p>");
            html.append("</body></html>");
            JSONObject result = AgentPdfRenderer.render(html.toString(), output);
            assertTrue(result.getInt("pages") > 1);
            assertTrue(result.getLong("bytes") > 1000);
            try (FileInputStream input = new FileInputStream(output)) {
                byte[] header = new byte[5];
                assertEquals(5, input.read(header));
                assertEquals("%PDF-", new String(header, StandardCharsets.US_ASCII));
            }
        } finally {
            output.delete();
        }
    }
}
