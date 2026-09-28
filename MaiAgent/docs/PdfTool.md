# generate_pdf

`generate_pdf` 把 Agent 工作区里的 UTF-8 HTML 文件排成 A4 PDF。模型先用 `write`
建立自包含 HTML，再调用：

```json
{"source_path":"report.html","output_path":"report.pdf"}
```

两个路径都必须留在会话工作区内。源文件限 10 MB；输出必须是尚不存在的 `.pdf`
文件，产物限 100 MB。工具按写文件操作进入审批流程，不使用 shell 或外部浏览器。
路径校验、读取 HTML、审批和 PDF 文件头检查由 MaiAgent 共用；宿主只实现排版回调。

MaiChat Desktop 使用 Qt 富文本 PDF 输出；iOS 使用 UIKit 的多页 HTML 打印排版；
Android 使用系统 HTML 文本排版和 `PdfDocument`。三端都支持中文、基本富文本和
分页，但复杂网页 CSS 的排版并不保证逐像素相同。图片应写成 HTML 内的 `data:`
URL；外部资源不属于工具输入契约。
