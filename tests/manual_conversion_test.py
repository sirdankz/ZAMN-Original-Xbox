"""Check page ordering and RGB565 conversion using only synthetic artwork."""
import importlib.util
import tempfile
import zipfile
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("convert_manual", root / "tools/convert_manual.py")
converter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(converter)


def pixel(path, x, y):
    raw = path.read_bytes()
    assert len(raw) == 1024 * 760 * 2
    offset = (y * 1024 + x) * 2
    return int.from_bytes(raw[offset:offset + 2], "little")


with tempfile.TemporaryDirectory() as temp:
    folder = Path(temp)
    assert converter.convert(folder) is False
    images = [Image.new("RGB", (100, 150) if i in (0, 10) else (200, 150),
                        "red" if i == 0 else "blue" if i == 10 else "green") for i in range(11)]
    pdf = folder / (converter.BASENAME + ".pdf")
    images[0].save(pdf, save_all=True, append_images=images[1:])
    assert converter.convert(folder)
    assert pixel(folder / "spread00.rgb565", 256, 380) == 0x001F  # back cover on left
    assert pixel(folder / "spread00.rgb565", 768, 380) == 0xF800  # front cover on right
    assert pixel(folder / "spread01.rgb565", 512, 380) == 0x0400
    previous = (folder / "spread00.rgb565").stat().st_mtime_ns
    assert converter.convert(folder)
    assert (folder / "spread00.rgb565").stat().st_mtime_ns == previous
    (folder / "spread00.rgb565").write_bytes(b"bad")
    assert converter.convert(folder)  # regenerate corrupted output
    assert pixel(folder / "spread00.rgb565", 768, 380) == 0xF800
    pdf.unlink()
    epub = folder / (converter.BASENAME + ".epub")
    with zipfile.ZipFile(epub, "w") as book:
        book.writestr("META-INF/container.xml", '<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OPS/book.opf"/></rootfiles></container>')
        manifest, spine = [], []
        for i in range(20):
            name = f"p{i:02d}"
            manifest.append(f'<item id="{name}" href="{name}.xhtml" media-type="application/xhtml+xml"/>')
            spine.append(f'<itemref idref="{name}"/>')
            book.writestr(f"OPS/{name}.xhtml", f'<html xmlns="http://www.w3.org/1999/xhtml"><body><img src="images/{name}.png"/></body></html>')
            import io
            image = Image.new("RGB", (100, 150), "red" if i == 0 else "blue" if i == 19 else "green")
            data = io.BytesIO()
            image.save(data, format="PNG")
            book.writestr(f"OPS/images/{name}.png", data.getvalue())
        book.writestr("OPS/book.opf", '<package xmlns="http://www.idpf.org/2007/opf"><manifest>' + ''.join(manifest) + '</manifest><spine>' + ''.join(spine) + '</spine></package>')
    assert converter.convert(folder)
    assert pixel(folder / "spread00.rgb565", 256, 380) == 0x001F
    assert pixel(folder / "spread00.rgb565", 768, 380) == 0xF800
    assert pixel(folder / "spread09.rgb565", 256, 380) == 0x0400
    saved = (folder / "spread00.rgb565").read_bytes()
    epub.write_bytes(b"invalid")
    try:
        converter.convert(folder)
        raise AssertionError("Malformed input must fail instead of silently using stale spreads")
    except zipfile.BadZipFile:
        pass
    assert (folder / "spread00.rgb565").read_bytes() == saved
    try:
        converter.spreads([Image.new("RGB", (100, 150))] * 9)
        raise AssertionError("Incomplete manual must fail")
    except ValueError:
        pass
print("Manual conversion: PDF/EPUB page order, RGB565, caching, repair and invalid input PASS")
