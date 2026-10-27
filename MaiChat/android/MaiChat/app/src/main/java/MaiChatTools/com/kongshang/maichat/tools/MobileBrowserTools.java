package com.kongshang.maichat.tools;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.net.Uri;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.webkit.CookieManager;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.webkit.WebStorage;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.nio.file.Files;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONArray;
import org.json.JSONObject;
import org.json.JSONTokener;

/** One visible page owned by a local Agent Session; no arbitrary model JavaScript. */
public final class MobileBrowserTools {
    private final Activity activity;
    private LinearLayout host, pane;
    private WebView page;
    private TextView heading;
    private String ownerSession = "", selectedSession = "", allowedHost = "";
    private String snapshotId = "", pageUrl = "";
    private final Map<String, String> observedLabels = new HashMap<>();
    private boolean visible, loaded;

    public MobileBrowserTools(Activity activity) { this.activity = activity; }

    // Ordinary App callers and the Agent use the same implementation.
    public JSONObject openPage(String sessionId, String url) throws Exception {
        return execute(new JSONObject().put("action", "open")
            .put("session_id", sessionId).put("url", url));
    }

    public JSONObject observePage(String sessionId) throws Exception {
        return execute(new JSONObject().put("action", "observe")
            .put("session_id", sessionId));
    }

    public JSONObject capturePage(String sessionId, String expectedUrl,
                                  String outputPath) throws Exception {
        return execute(new JSONObject().put("action", "screenshot")
            .put("session_id", sessionId).put("expected_url", expectedUrl)
            .put("output_path", outputPath));
    }

    public JSONObject actOnPage(String sessionId, String action, String expectedUrl,
                                String snapshotId, String elementId,
                                String expectedLabel, String text) throws Exception {
        JSONObject input = new JSONObject().put("action", action)
            .put("session_id", sessionId).put("expected_url", expectedUrl)
            .put("snapshot_id", snapshotId).put("element_id", elementId)
            .put("expected_label", expectedLabel);
        if (action.equals("type")) input.put("text", text);
        return execute(input);
    }

    public void attach(LinearLayout container) {
        host = container;
        if (pane != null) showPane();
    }

    public void detach(LinearLayout container) {
        if (host != container) return;
        if (pane != null && pane.getParent() == host) host.removeView(pane);
        host = null;
        visible = false;
    }

    public void setVisible(boolean value) { visible = value; }

    public void sessionChanged(String session, boolean remote) {
        selectedSession = remote ? "" : session;
        if (page != null && !ownerSession.equals(selectedSession)) close();
    }

    public void close() {
        if (host != null) {
            host.removeAllViews();
            host.setVisibility(View.GONE);
        }
        if (page != null) {
            page.stopLoading();
            page.destroy();
            page = null;
        }
        pane = null;
        heading = null;
        ownerSession = "";
        allowedHost = "";
        snapshotId = "";
        pageUrl = "";
        loaded = false;
        observedLabels.clear();
    }

    public void onDestroy() {
        activity.runOnUiThread(this::close);
    }

    public JSONObject execute(JSONObject arguments) throws Exception {
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IllegalStateException("网页工具不能阻塞界面线程");
        CountDownLatch finished = new CountDownLatch(1);
        AtomicReference<JSONObject> output = new AtomicReference<>();
        AtomicReference<Exception> failure = new AtomicReference<>();
        Completion completion = new Completion() {
            @Override public void success(JSONObject value) {
                output.set(value);
                finished.countDown();
            }
            @Override public void failure(Exception error) {
                failure.set(error);
                finished.countDown();
            }
        };
        activity.runOnUiThread(() -> {
            try { perform(arguments, completion); }
            catch (Exception error) { completion.failure(error); }
        });
        if (!finished.await(30, TimeUnit.SECONDS))
            throw new IllegalStateException("等待网页操作超时；请先检查当前网页状态");
        if (failure.get() != null) throw failure.get();
        return output.get();
    }

    private interface Completion {
        void success(JSONObject value);
        void failure(Exception error);
    }

    private void perform(JSONObject input, Completion done) throws Exception {
        String action = input.optString("action", "");
        String session = input.optString("session_id", "");
        if (session.isEmpty()) throw new IllegalArgumentException("网页操作缺少所属 Session");
        if (action.equals("close")) {
            if (!session.equals(ownerSession))
                throw new IllegalArgumentException("此 Session 没有打开的网页");
            close();
            done.success(new JSONObject().put("state", "closed"));
            return;
        }
        if (!visible || host == null || !session.equals(selectedSession))
            throw new IllegalStateException("请先在前台打开此设备的创作中心 Session");
        if (action.equals("open")) {
            String url = input.optString("url", "");
            Uri parsed = Uri.parse(url);
            String scheme = parsed.getScheme(), nextHost = parsed.getHost();
            if (url.length() > 2048 || scheme == null || nextHost == null ||
                (!scheme.equals("http") && !scheme.equals("https")) ||
                parsed.getUserInfo() != null)
                throw new IllegalArgumentException("请提供不含账号密码的 HTTP(S) 网址");
            close();
            ownerSession = session;
            allowedHost = nextHost.toLowerCase(java.util.Locale.ROOT);
            pageUrl = url;
            CookieManager.getInstance().removeAllCookies(removed -> {
                try {
                    if (!visible || host == null || !session.equals(selectedSession) ||
                        !session.equals(ownerSession)) {
                        close();
                        done.failure(new IllegalStateException("网页打开前 Session 已切换"));
                        return;
                    }
                    CookieManager.getInstance().flush();
                    WebStorage.getInstance().deleteAllData();
                    createPage();
                    showPane();
                    page.loadUrl(url);
                    done.success(new JSONObject().put("state", "loading").put("url", url)
                        .put("next_step", "Call observe after the page finishes loading."));
                } catch (Exception error) { done.failure(error); }
            });
            return;
        }
        if (page == null || !session.equals(ownerSession))
            throw new IllegalArgumentException("此 Session 没有打开的网页");
        if (action.equals("observe")) {
            if (!loaded || page.getProgress() < 100) {
                done.success(new JSONObject().put("state", "loading").put("url", pageUrl));
                return;
            }
            String nonce = UUID.randomUUID().toString().replace("-", "");
            page.evaluateJavascript(snapshotScript(nonce), result -> {
                try {
                    Object decoded = new JSONTokener(result).nextValue();
                    if (!(decoded instanceof String))
                        throw new IllegalStateException("无法读取网页结构");
                    JSONObject snapshot = new JSONObject((String) decoded);
                    JSONArray elements = snapshot.getJSONArray("elements");
                    observedLabels.clear();
                    for (int index = 0; index < elements.length(); ++index) {
                        JSONObject element = elements.getJSONObject(index);
                        observedLabels.put(element.getString("id"), element.getString("label"));
                    }
                    snapshotId = nonce;
                    pageUrl = snapshot.optString("url", pageUrl);
                    done.success(snapshot.put("snapshot_id", nonce).put("state", "ready"));
                } catch (Exception error) { done.failure(error); }
            });
            return;
        }
        if (action.equals("screenshot")) {
            if (!loaded || page.getProgress() < 100 || page.getWidth() <= 0 ||
                page.getHeight() <= 0 ||
                !input.optString("expected_url", "").equals(page.getUrl()))
                throw new IllegalStateException("网页尚未加载完成或已离开确认的网址");
            String path = input.optString("output_path", "");
            File destination = new File(path).getCanonicalFile();
            File root = activity.getNoBackupFilesDir().getCanonicalFile();
            if (!destination.getPath().startsWith(root.getPath() + File.separator) ||
                !destination.getName().endsWith(".png"))
                throw new IllegalArgumentException("网页截图路径无效");
            Bitmap bitmap = Bitmap.createBitmap(page.getWidth(), page.getHeight(),
                Bitmap.Config.ARGB_8888);
            page.draw(new Canvas(bitmap));
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            boolean encoded = bitmap.compress(Bitmap.CompressFormat.PNG, 100, bytes);
            int width = bitmap.getWidth(), height = bitmap.getHeight();
            bitmap.recycle();
            if (!encoded || bytes.size() == 0 || bytes.size() > 20 * 1024 * 1024)
                throw new IllegalStateException("网页截图为空或超出大小限制");
            Files.write(destination.toPath(), bytes.toByteArray());
            done.success(new JSONObject().put("state", "captured")
                .put("url", pageUrl).put("width", width).put("height", height));
            return;
        }
        if (action.equals("click") || action.equals("type")) {
            String requested = input.optString("snapshot_id", "");
            String id = input.optString("element_id", "");
            String label = input.optString("expected_label", "");
            if (!loaded || page.getProgress() < 100 || requested.isEmpty() ||
                !requested.equals(snapshotId) || !label.equals(observedLabels.get(id)) ||
                !input.optString("expected_url", "").equals(page.getUrl()))
                throw new IllegalArgumentException("网页已变化，请重新读取后选择目标");
            page.evaluateJavascript(actionScript(action, id, label,
                input.optString("text", "")), result -> {
                try {
                    Object decoded = new JSONTokener(result).nextValue();
                    if (!"dispatched".equals(decoded))
                        throw new IllegalArgumentException("目标已变化或是密码输入框，请重新读取网页");
                    snapshotId = "";
                    observedLabels.clear();
                    done.success(new JSONObject().put("state", "dispatched")
                        .put("action", action).put("verified", false)
                        .put("next_step", "Call observe to verify the page state."));
                } catch (Exception error) { done.failure(error); }
            });
            return;
        }
        throw new IllegalArgumentException("未知的网页操作");
    }

    private void createPage() {
        pane = new LinearLayout(activity);
        pane.setOrientation(LinearLayout.VERTICAL);
        pane.setBackgroundColor(Color.WHITE);
        LinearLayout bar = new LinearLayout(activity);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setPadding(dp(12), dp(5), dp(10), dp(5));
        heading = new TextView(activity);
        heading.setText(allowedHost);
        heading.setTextColor(Color.rgb(30, 41, 59));
        heading.setSingleLine(true);
        bar.addView(heading, new LinearLayout.LayoutParams(0, dp(38), 1));
        Button closeButton = new Button(activity);
        closeButton.setText("关闭网页");
        closeButton.setOnClickListener(view -> close());
        bar.addView(closeButton, new LinearLayout.LayoutParams(dp(92), dp(38)));
        pane.addView(bar, new LinearLayout.LayoutParams(-1, dp(48)));
        page = new WebView(activity);
        WebSettings settings = page.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setJavaScriptCanOpenWindowsAutomatically(false);
        settings.setSupportMultipleWindows(false);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        CookieManager.getInstance().setAcceptThirdPartyCookies(page, false);
        page.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                if (!request.isForMainFrame()) return false;
                Uri url = request.getUrl();
                String host = url.getHost(), scheme = url.getScheme();
                boolean allowed = host != null && host.equalsIgnoreCase(allowedHost) &&
                    ("http".equals(scheme) || "https".equals(scheme));
                if (!allowed && heading != null) heading.setText("已阻止离开当前网站");
                return !allowed;
            }
            @Override public void onPageStarted(WebView view, String url, Bitmap icon) {
                loaded = false;
                snapshotId = "";
                observedLabels.clear();
                pageUrl = url;
                if (heading != null) heading.setText("正在加载 " + allowedHost);
            }
            @Override public void onPageFinished(WebView view, String url) {
                loaded = true;
                pageUrl = url;
                if (heading != null) heading.setText(view.getTitle() == null ? allowedHost : view.getTitle());
            }
        });
        page.setWebChromeClient(new WebChromeClient() {
            @Override public void onReceivedTitle(WebView view, String title) {
                if (heading != null) heading.setText(title);
            }
        });
        pane.addView(page, new LinearLayout.LayoutParams(-1, 0, 1));
    }

    private void showPane() {
        if (host == null || pane == null) return;
        if (pane.getParent() instanceof ViewGroup)
            ((ViewGroup) pane.getParent()).removeView(pane);
        host.removeAllViews();
        host.addView(pane, new LinearLayout.LayoutParams(-1, -1));
        host.setVisibility(View.VISIBLE);
    }

    private int dp(float value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    private static String snapshotScript(String nonce) {
        return "(() => {" +
            "const visible=e=>{const r=e.getBoundingClientRect(),s=getComputedStyle(e);" +
            "return r.width>0&&r.height>0&&s.visibility!=='hidden'&&s.display!=='none'};" +
            "const label=e=>(e.getAttribute('aria-label')||e.innerText||" +
            "e.getAttribute('placeholder')||e.getAttribute('name')||" +
            "e.getAttribute('title')||e.tagName.toLowerCase()).trim().slice(0,160);" +
            "const candidates=Array.from(document.querySelectorAll(" +
            "'a[href],button,input,textarea,select,[role=\"button\"]')).filter(visible).slice(0,80);" +
            "const nonce=" + JSONObject.quote(nonce) + ";" +
            "const elements=candidates.map((e,i)=>{const id='mai_'+nonce+'_'+i;" +
            "e.setAttribute('data-maichat-agent-id',id);return {id,label:label(e)," +
            "tag:e.tagName.toLowerCase(),type:(e.getAttribute('type')||'').toLowerCase()}});" +
            "return JSON.stringify({url:location.href,title:document.title," +
            "text:(document.body?.innerText||'').slice(0,20000),elements})})()";
    }

    private static String actionScript(String action, String id, String label, String text) {
        return "(() => {const id=" + JSONObject.quote(id) + ",expected=" +
            JSONObject.quote(label) + ",action=" + JSONObject.quote(action) + ";" +
            "const e=Array.from(document.querySelectorAll('[data-maichat-agent-id]'))" +
            ".find(item=>item.getAttribute('data-maichat-agent-id')===id);" +
            "if(!e)return 'stale';const actual=(e.getAttribute('aria-label')||" +
            "e.innerText||e.getAttribute('placeholder')||e.getAttribute('name')||" +
            "e.getAttribute('title')||e.tagName.toLowerCase()).trim().slice(0,160);" +
            "if(actual!==expected)return 'stale';" +
            "if(action==='click'){setTimeout(()=>e.click(),0);return 'dispatched'}" +
            "const type=(e.getAttribute('type')||'text').toLowerCase();" +
            "if((e.tagName!=='INPUT'&&e.tagName!=='TEXTAREA')||" +
            "['password','hidden','file','checkbox','radio','submit'].includes(type)||" +
            "['current-password','new-password','one-time-code'].includes(" +
            "(e.getAttribute('autocomplete')||'').toLowerCase()))return 'unsupported';" +
            "e.value=" + JSONObject.quote(text) + ";" +
            "e.dispatchEvent(new Event('input',{bubbles:true}));" +
            "e.dispatchEvent(new Event('change',{bubbles:true}));return 'dispatched'})()";
    }
}
