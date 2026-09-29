# generate_pdf

Agent 可以一次调用直接把 Markdown 正文生成 A4 PDF，不必先创建 HTML 文件：

```json
{"content":"# 项目报告\n\n| 项目 | 结果 |\n| --- | --- |\n| 构建 | 通过 |","output_path":"report.pdf"}
```

`content` 使用 UTF-8，支持标题、列表、表格、强调等 GitHub 风格 Markdown。MaiAgent 使用仓库内的 md4c 将正文转换成内存中的 HTML，然后把 HTML 交给宿主的 PDF 排版回调；中间过程不写 HTML 文件。原有 `source_path` 指向工作区 HTML 文件的调用仍能执行，供旧会话兼容，但不再展示给模型作为新调用参数。

工具遵守写文件审批。输出必须是工作区内尚不存在的 `.pdf` 文件；正文限制 10 MB，生成的 PDF 限制 100 MB。Markdown 中的原始 HTML 会作为文字处理，图片只接受 `data:image/...;base64,...` 形式，不读取外部地址。渲染失败或取消时清理输出，成功后核对 PDF 文件头和大小。

MaiChat Desktop 使用 Qt 的 `QTextDocument` 与 `QPdfWriter`；iOS 使用 UIKit 的 HTML 打印排版；Android 使用 `Html.fromHtml`、`StaticLayout` 和系统 `PdfDocument`。三端支持基本富文本与分页，但复杂 CSS 和表格不保证逐像素一致。
