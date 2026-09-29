package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.assertNull;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.json.JSONObject;

public class PdfPreviewDialogTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test public void onlyPreviewsPdfInsideTheSelectedWorkspace() throws Exception {
        File workspace = temporary.newFolder("Workspace");
        File pdf = new File(workspace, "report.pdf");
        try (FileOutputStream output = new FileOutputStream(pdf)) {
            output.write("%PDF-1.4\n%%EOF\n".getBytes(StandardCharsets.US_ASCII));
        }
        assertEquals(pdf.getCanonicalFile(), PdfPreviewDialog.validate(pdf, workspace));
        File cached = new File(workspace, "cached-file");
        try (FileOutputStream output = new FileOutputStream(cached)) {
            output.write("%PDF-1.4\n%%EOF\n".getBytes(StandardCharsets.US_ASCII));
        }
        assertEquals(cached.getCanonicalFile(), PdfPreviewDialog.validate(cached, workspace));

        File outside = temporary.newFile("outside.pdf");
        assertThrows(IOException.class, () -> PdfPreviewDialog.validate(outside, workspace));
        try (FileOutputStream output = new FileOutputStream(pdf, false)) {
            output.write("not a PDF\n".getBytes(StandardCharsets.US_ASCII));
        }
        assertThrows(IOException.class, () -> PdfPreviewDialog.validate(pdf, workspace));
    }

    @Test public void completedPdfToolResultBecomesPreviewableArtifact() throws Exception {
        JSONObject part = new JSONObject().put("tool", "generate_pdf")
            .put("state", "completed")
            .put("output", "{\"path\":\"report.pdf\",\"mime_type\":\"application/pdf\",\"bytes\":1234}");
        assertEquals("report.pdf", AIAssistantPanel.pdfPathFromToolOutput(part));
        part.put("state", "error");
        assertNull(AIAssistantPanel.pdfPathFromToolOutput(part));
        part.put("state", "completed").put("tool", "other_tool");
        assertNull(AIAssistantPanel.pdfPathFromToolOutput(part));
        part.put("tool", "generate_pdf").put("output", "Created older.pdf (1234 bytes).");
        assertEquals("older.pdf", AIAssistantPanel.pdfPathFromToolOutput(part));
    }
}
