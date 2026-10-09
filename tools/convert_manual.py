"""Convert a locally supplied scanned manual; no commercial assets are included."""
import argparse
import array
import hashlib
import io
import json
import posixpath
import sys
import tempfile
import zipfile
from pathlib import Path
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET

WIDTH, HEIGHT = 1024, 760
BASENAME = "Zombies Ate My Neighbors"


def local_path(base, href):
    url = urlsplit(href)
    if url.scheme or url.netloc:
        raise ValueError("EPUB images must be stored inside the EPUB")
    path = posixpath.normpath(posixpath.join(posixpath.dirname(base), unquote(url.path)))
    if path.startswith("../") or path.startswith("/"):
        raise ValueError("Invalid EPUB asset path")
    return path


def epub_pages(source):
    from PIL import Image
    with zipfile.ZipFile(source) as book:
        container = ET.fromstring(book.read("META-INF/container.xml"))
        opf = next(e for e in container.iter() if e.tag.endswith("}rootfile")).attrib["full-path"]
        package = ET.fromstring(book.read(opf))
        items = {e.attrib["id"]: e for e in package.iter() if e.tag.endswith("}item")}
        spine = [e.attrib["idref"] for e in package.iter() if e.tag.endswith("}itemref")]
        pages = []
        seen = set()
        for item_id in spine:
            item = items[item_id]
            if "nav" in item.attrib.get("properties", "").split():
                continue
            name = local_path(opf, item.attrib["href"])
            if item.attrib.get("media-type", "").startswith("image/"):
                images = [name]
            else:
                doc = ET.fromstring(book.read(name))
                images = []
                for element in doc.iter():
                    tag = element.tag.rsplit("}", 1)[-1]
                    href = element.attrib.get("src") if tag == "img" else None
                    if tag == "image":
                        href = element.attrib.get("href") or element.attrib.get("{http://www.w3.org/1999/xlink}href")
                    if href:
                        images.append(local_path(name, href))
            for name in images:
                if name in seen:
                    continue
                seen.add(name)
                with Image.open(io.BytesIO(book.read(name))) as image:
                    pages.append(image.convert("RGB"))
        return pages


def pdf_pages(source):
    import pypdfium2 as pdfium
    with pdfium.PdfDocument(str(source)) as doc:
        if len(doc) not in (10, 11, 20):
            raise ValueError("Expected 11 scanned PDF pages, 20 single pages, or 10 prepared spreads")
        pages = []
        for index in range(len(doc)):
            page = doc[index]
            width, height = page.get_size()
            bitmap = page.render(scale=max(WIDTH / width, HEIGHT / height))
            pages.append(bitmap.to_pil().convert("RGB"))
            bitmap.close()
            page.close()
        return pages


def fit(image, width=WIDTH):
    from PIL import Image, ImageOps
    image = ImageOps.contain(image, (width, HEIGHT), Image.Resampling.LANCZOS)
    canvas = Image.new("RGB", (width, HEIGHT), "white")
    canvas.paste(image, ((width - image.width) // 2, (HEIGHT - image.height) // 2))
    return canvas


def pair(left, right):
    from PIL import Image
    canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
    canvas.paste(fit(left, WIDTH // 2), (0, 0))
    canvas.paste(fit(right, WIDTH // 2), (WIDTH // 2, 0))
    return canvas


def spreads(pages):
    if len(pages) == 10:
        if any(p.width / p.height < 1.1 for p in pages):
            raise ValueError("Ten-page input must already contain landscape spreads")
        return [fit(p) for p in pages]
    if len(pages) == 11:
        if any(p.width / p.height < 1.1 for p in pages[1:-1]):
            raise ValueError("Eleven-page input must have front cover, nine landscape spreads, back cover")
        return [pair(pages[-1], pages[0])] + [fit(p) for p in pages[1:-1]]
    if len(pages) == 20:
        return [pair(pages[-1], pages[0])] + [pair(pages[i], pages[i + 1]) for i in range(1, 19, 2)]
    raise ValueError("Expected 11 scan images, 20 single pages, or 10 prepared spreads; text-only EPUBs are unsupported")


def rgb565(image):
    pixels = array.array("H", (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) for r, g, b in image.getdata()))
    if sys.byteorder != "little":
        pixels.byteswap()
    return pixels.tobytes()


def convert(folder):
    source = next((folder / (BASENAME + extension) for extension in (".pdf", ".epub")
                   if (folder / (BASENAME + extension)).is_file()), None)
    if source is None:
        return False
    digest = hashlib.sha256(b"manual-converter-v1\0" + source.read_bytes()).hexdigest()
    stamp = folder / ".generated-manual.json"
    names = [f"spread{i:02d}.rgb565" for i in range(10)]
    if stamp.is_file():
        try:
            previous = json.loads(stamp.read_text())
            if previous["source_sha256"] == digest and all(
                (folder / name).is_file() and (folder / name).stat().st_size == WIDTH * HEIGHT * 2
                and hashlib.sha256((folder / name).read_bytes()).hexdigest() == previous["outputs"][name]
                for name in names
            ):
                print("Optional manual: generated spreads are current.")
                return True
        except (ValueError, KeyError, OSError):
            pass
    pages = pdf_pages(source) if source.suffix == ".pdf" else epub_pages(source)
    images = spreads(pages)
    hashes = {}
    # Validate and render the full set before replacing any local spreads.
    with tempfile.TemporaryDirectory(prefix=".manual-convert-", dir=folder) as staging:
        for name, image in zip(names, images):
            data = rgb565(image)
            (Path(staging) / name).write_bytes(data)
            hashes[name] = hashlib.sha256(data).hexdigest()
        for name in names:
            (Path(staging) / name).replace(folder / name)
    stamp.write_text(json.dumps({"source_sha256": digest, "outputs": hashes}, indent=2) + "\n")
    print(f"Optional manual: converted {source.name} into ten RGB565 spreads.")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manual-dir", type=Path, default=Path(__file__).resolve().parents[1] / "manual")
    args = parser.parse_args()
    try:
        convert(args.manual_dir)
        return 0
    except ImportError as error:
        print(f"Manual omitted: missing conversion dependency ({error.name}). Install tools/manual-requirements.txt.", file=sys.stderr)
    except Exception as error:
        print(f"Manual omitted: {error}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
