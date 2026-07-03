import sys
import pypdf

paths = sys.argv[1:]
for path in paths:
    out_txt = path.replace('.pdf', '.txt')
    try:
        reader = pypdf.PdfReader(path)
        total = len(reader.pages)
        print(f"{path}: {total} pages")
        with open(out_txt, 'w', encoding='utf-8') as f:
            f.write(f"===== {path} =====\n")
            f.write(f"Total pages: {total}\n\n")
            # 取前 8 页
            for i, page in enumerate(reader.pages[:8]):
                f.write(f"\n----- Page {i+1} -----\n")
                text = page.extract_text() or ""
                f.write(text + "\n")
        print(f"  -> wrote {out_txt}")
    except Exception as e:
        print(f"ERROR: {e}")