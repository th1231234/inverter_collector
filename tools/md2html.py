"""把 Markdown 文档转成可导航、可搜索的单文件网页。

用于把技术笔记/手册变成"看得下去"的形态：左侧目录树 + 右侧正文 + 顶部搜索。
生成的是单个 .html 文件，双击就能在浏览器打开，不需要任何服务器。

用法：
    python md2html.py D:\\work\\MQTT\\MQTT.md
    python md2html.py 输入.md 输出.html "页面标题"
"""

import os
import re
import sys

import markdown

TEMPLATE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
:root {{
  --bg: #ffffff;
  --bg-soft: #f7f8fa;
  --bg-code: #f6f8fa;
  --fg: #1f2328;
  --fg-muted: #656d76;
  --border: #d8dee4;
  --accent: #0969da;
  --accent-soft: #ddf4ff;
  --maxw: 880px;
}}
* {{ box-sizing: border-box; }}
html {{ scroll-behavior: smooth; }}
body {{
  margin: 0;
  background: var(--bg);
  color: var(--fg);
  font-family: -apple-system, "Segoe UI", "Microsoft YaHei", "PingFang SC", sans-serif;
  font-size: 15px;
  line-height: 1.75;
}}
#wrap {{ display: flex; align-items: flex-start; }}

/* ---------- 左侧目录 ---------- */
aside {{
  position: sticky;
  top: 0;
  width: 290px;
  min-width: 290px;
  height: 100vh;
  overflow-y: auto;
  border-right: 1px solid var(--border);
  background: var(--bg-soft);
  padding: 18px 14px 60px;
}}
aside h1 {{
  font-size: 15px;
  font-weight: 600;
  margin: 0 0 12px 4px;
  letter-spacing: .2px;
}}
#q {{
  width: 100%;
  padding: 7px 10px;
  font-size: 13px;
  border: 1px solid var(--border);
  border-radius: 7px;
  background: #fff;
  margin-bottom: 14px;
  outline: none;
  font-family: inherit;
}}
#q:focus {{ border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); }}
#hits {{ font-size: 12px; color: var(--fg-muted); margin: -6px 0 10px 4px; min-height: 16px; }}

.toc {{ font-size: 13px; }}
.toc ul {{ list-style: none; margin: 0; padding-left: 12px; }}
.toc > ul {{ padding-left: 0; }}
.toc li {{ margin: 1px 0; }}
.toc a {{
  display: block;
  padding: 3px 8px;
  color: var(--fg);
  text-decoration: none;
  border-radius: 5px;
  border-left: 2px solid transparent;
}}
.toc a:hover {{ background: #eef1f4; }}
.toc a.active {{ background: var(--accent-soft); color: var(--accent); border-left-color: var(--accent); font-weight: 500; }}
.toc ul ul {{ font-size: 12.5px; color: var(--fg-muted); }}

/* ---------- 右侧正文 ---------- */
main {{ flex: 1; min-width: 0; }}
article {{
  max-width: var(--maxw);
  margin: 0 auto;
  padding: 40px 48px 120px;
}}
article h1, article h2, article h3, article h4, article h5, article h6 {{
  line-height: 1.4;
  margin: 1.9em 0 .7em;
  scroll-margin-top: 20px;
}}
article h1 {{ font-size: 30px; font-weight: 650; margin-top: 0; padding-bottom: .3em; border-bottom: 1px solid var(--border); }}
article h2 {{ font-size: 23px; font-weight: 650; padding-bottom: .3em; border-bottom: 1px solid var(--border); }}
article h3 {{ font-size: 18px; font-weight: 600; color: #1f2328; }}
article h4 {{ font-size: 16px; font-weight: 600; color: #24292f; }}
article h5, article h6 {{ font-size: 14.5px; font-weight: 600; color: var(--fg-muted); }}
article p {{ margin: 0 0 15px; }}
article a {{ color: var(--accent); text-decoration: none; }}
article a:hover {{ text-decoration: underline; }}
article ul, article ol {{ padding-left: 26px; margin: 0 0 15px; }}
article li {{ margin: 4px 0; }}
article strong {{ font-weight: 600; }}

blockquote {{
  margin: 0 0 15px;
  padding: 8px 16px;
  color: var(--fg-muted);
  border-left: 3px solid var(--border);
  background: var(--bg-soft);
  border-radius: 0 6px 6px 0;
}}

/* 行内代码 */
article code {{
  font-family: "Cascadia Code", Consolas, "Courier New", monospace;
  font-size: 13px;
  background: rgba(175,184,193,.2);
  padding: 2px 6px;
  border-radius: 5px;
}}

/* 代码块 */
.codebox {{ position: relative; margin: 0 0 18px; }}
.codebox pre {{
  margin: 0;
  padding: 14px 16px;
  overflow-x: auto;
  background: var(--bg-code);
  border: 1px solid var(--border);
  border-radius: 8px;
  font-size: 12.5px;
  line-height: 1.6;
}}
.codebox pre code {{
  background: none;
  padding: 0;
  font-size: 12.5px;
  white-space: pre;
}}
.codebox .copy {{
  position: absolute;
  top: 8px;
  right: 8px;
  font-size: 11.5px;
  padding: 3px 9px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: #fff;
  color: var(--fg-muted);
  cursor: pointer;
  opacity: 0;
  transition: opacity .15s;
  font-family: inherit;
}}
.codebox:hover .copy {{ opacity: 1; }}
.codebox .copy.done {{ color: #1a7f37; border-color: #1a7f37; }}
code.language-tag {{
  position: absolute;
  top: 8px;
  left: 12px;
  font-size: 10.5px;
  color: #8b949e;
  text-transform: uppercase;
  letter-spacing: .5px;
  pointer-events: none;
}}
.codebox pre {{ padding-top: 30px; }}

/* 表格 */
table {{
  border-collapse: collapse;
  margin: 0 0 18px;
  font-size: 14px;
  width: auto;
  max-width: 100%;
  display: block;
  overflow-x: auto;
}}
th, td {{
  border: 1px solid var(--border);
  padding: 7px 13px;
  text-align: left;
  vertical-align: top;
}}
th {{ background: var(--bg-soft); font-weight: 600; white-space: nowrap; }}
tbody tr:nth-child(even) {{ background: #fbfcfd; }}

img {{
  max-width: 100%;
  border: 1px solid var(--border);
  border-radius: 8px;
  margin: 6px 0;
}}
.imgmiss {{
  display: inline-block;
  padding: 10px 14px;
  background: #fff8e6;
  border: 1px dashed #d4a72c;
  border-radius: 8px;
  font-size: 13px;
  color: #7a5c00;
}}

hr {{ border: none; border-top: 1px solid var(--border); margin: 30px 0; }}

#top {{
  position: fixed;
  right: 26px;
  bottom: 26px;
  width: 40px;
  height: 40px;
  border-radius: 50%;
  border: 1px solid var(--border);
  background: #fff;
  color: var(--fg-muted);
  cursor: pointer;
  font-size: 16px;
  box-shadow: 0 2px 8px rgba(0,0,0,.08);
  opacity: 0;
  pointer-events: none;
  transition: opacity .2s;
}}
#top.show {{ opacity: 1; pointer-events: auto; }}

@media (max-width: 900px) {{
  aside {{ display: none; }}
  article {{ padding: 24px 18px 90px; }}
}}
</style>
</head>
<body>
<div id="wrap">
  <aside>
    <h1>{side_title}</h1>
    <input id="q" type="search" placeholder="搜索目录标题…" autocomplete="off">
    <div id="hits"></div>
    {toc}
  </aside>
  <main>
    <article id="content">
{body}
    </article>
  </main>
</div>
<button id="top" title="回到顶部">↑</button>

<script>
(function () {{
  var q = document.getElementById('q');
  var links = Array.prototype.slice.call(document.querySelectorAll('.toc a'));
  var hits = document.getElementById('hits');

  function filter() {{
    var s = q.value.trim().toLowerCase();
    var n = 0;
    links.forEach(function (a) {{
      var hit = !s || a.textContent.toLowerCase().indexOf(s) !== -1;
      a.parentElement.style.display = hit ? '' : 'none';
      if (hit && s) n++;
    }});
    // 标题被隐藏时，父级 ul 可能全空，这里不管，保持简单
    hits.textContent = s ? ('匹配 ' + n + ' 项') : ('共 ' + links.length + ' 个标题');
  }}
  q.addEventListener('input', filter);
  q.addEventListener('keydown', function (e) {{
    if (e.key === 'Enter') {{
      var first = links.filter(function (a) {{ return a.parentElement.style.display !== 'none'; }})[0];
      if (first) first.click();
    }}
  }});
  filter();

  // 代码块：加语言标签和复制按钮
  document.querySelectorAll('pre').forEach(function (pre) {{
    var code = pre.querySelector('code');
    if (!code) return;
    var box = document.createElement('div');
    box.className = 'codebox';
    pre.parentNode.insertBefore(box, pre);
    box.appendChild(pre);

    var m = /language-([\\w+-]+)/.exec(code.className || '');
    if (m) {{
      var tag = document.createElement('span');
      tag.className = 'language-tag';
      tag.textContent = m[1];
      box.appendChild(tag);
    }}

    var btn = document.createElement('button');
    btn.className = 'copy';
    btn.textContent = '复制';
    btn.addEventListener('click', function () {{
      var txt = code.innerText;
      if (navigator.clipboard) {{
        navigator.clipboard.writeText(txt);
      }} else {{
        var ta = document.createElement('textarea');
        ta.value = txt; document.body.appendChild(ta); ta.select();
        document.execCommand('copy'); ta.remove();
      }}
      btn.textContent = '已复制';
      btn.classList.add('done');
      setTimeout(function () {{ btn.textContent = '复制'; btn.classList.remove('done'); }}, 1400);
    }});
    box.appendChild(btn);
  }});

  // 图片丢失时给个提示，而不是留一个破图标
  document.querySelectorAll('img').forEach(function (img) {{
    img.addEventListener('error', function () {{
      var miss = document.createElement('span');
      miss.className = 'imgmiss';
      miss.textContent = '图片缺失：' + (img.getAttribute('src') || '');
      img.replaceWith(miss);
    }});
  }});

  // 目录高亮当前章节
  var heads = Array.prototype.slice.call(document.querySelectorAll('article h2, article h3, article h4'));
  function onScroll() {{
    var y = window.scrollY + 90;
    var cur = null;
    heads.forEach(function (h) {{ if (h.offsetTop <= y) cur = h; }});
    links.forEach(function (a) {{
      a.classList.toggle('active', cur && a.getAttribute('href') === '#' + cur.id);
    }});
    document.getElementById('top').classList.toggle('show', window.scrollY > 400);
  }}
  window.addEventListener('scroll', onScroll, {{ passive: true }});
  onScroll();

  document.getElementById('top').addEventListener('click', function () {{
    window.scrollTo({{ top: 0, behavior: 'smooth' }});
  }});
}})();
</script>
</body>
</html>
"""


def cn_slugify(value, separator):
    """默认 slugify 会把中文全部丢掉（锚点变成 #1emqx），这里保留中文和数字。"""
    value = value.strip().lower()
    value = re.sub(r"[^\w\u4e00-\u9fff]+", separator, value)
    value = value.strip(separator)
    return value or "section"


def convert(md_path, out_path, title=None):
    with open(md_path, encoding="utf-8") as f:
        text = f.read()

    # 文档里大量 ```dat / ```text 这类非标准语言名，markdown 仍会正常渲染
    md = markdown.Markdown(
        extensions=["fenced_code", "tables", "toc", "codehilite", "sane_lists"],
        extension_configs={
            "toc": {"toc_depth": "1-4", "permalink": False, "slugify": cn_slugify},
            "codehilite": {"noclasses": True, "guess_lang": False,
                           "pygments_style": "friendly"},
        },
    )
    body = md.convert(text)
    toc_html = md.toc

    if title is None:
        m = re.search(r"^#\s+(.+)$", text, re.M)
        title = m.group(1).strip() if m else os.path.basename(md_path)

    # 图片路径原样保留（相对 md 所在目录），HTML 会和 md 放在同一目录
    html = TEMPLATE.format(title=title, side_title=title, toc=toc_html, body=body)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(html)

    size = os.path.getsize(out_path) / 1024
    heads = len(re.findall(r"<h[1-6][^>]*id=", body))
    print("生成成功: %s" % out_path)
    print("  标题数: %d" % heads)
    print("  文件大小: %.1f KB" % size)
    return out_path


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + ".html"
    ttl = sys.argv[3] if len(sys.argv) > 3 else None
    convert(src, dst, ttl)
