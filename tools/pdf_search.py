#!/usr/bin/env python3
"""
PDF 关键词搜索脚本：找包含指定关键词的页码 + 提取该页内容
用法: python pdf_search.py <pdf_path> <keyword1> [keyword2 ...]
"""
import sys
import pypdf

if len(sys.argv) < 3:
    print("用法: python pdf_search.py <pdf_path> <keyword1> [keyword2 ...]")
    sys.exit(1)

pdf_path = sys.argv[1]
keywords = sys.argv[2:]

try:
    reader = pypdf.PdfReader(pdf_path)
    total = len(reader.pages)
    print(f"PDF: {pdf_path} ({total} pages)")

    for kw in keywords:
        print(f"\n=== 搜索关键词: {kw!r} ===")
        found = []
        for i, page in enumerate(reader.pages):
            text = page.extract_text() or ""
            if kw.lower() in text.lower():
                found.append(i + 1)
        if found:
            print(f"  命中页: {found[:10]}{'...' if len(found) > 10 else ''} (共 {len(found)} 页)")
        else:
            print(f"  未找到")
except Exception as e:
    print(f"ERROR: {e}")
    sys.exit(1)
