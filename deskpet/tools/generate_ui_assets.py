from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]  # sketch root; this script lives in tools/
SCALE = 3


def array(name: str, values: list[int], kind: str) -> str:
    rows = [", ".join(str(value) for value in values[index:index + 24]) for index in range(0, len(values), 24)]
    return f"inline constexpr {kind} {name}[] = {{\n" + ",\n".join(rows) + "\n};\n"


def font_asset(name: str, size: int, characters: str, filename: str) -> str:
    font = ImageFont.truetype(str(Path("C:/Windows/Fonts") / filename), size * SCALE)
    pixels: list[int] = []
    glyphs: list[str] = []
    for character in characters:
        advance = round(font.getlength(character) / SCALE)
        bounds = font.getbbox(character, anchor="ls")
        width = max(1, (bounds[2] + SCALE - 1) // SCALE)
        height = size + 4
        image = Image.new("L", (width * SCALE, height * SCALE))
        ImageDraw.Draw(image).text((0, round(size * SCALE * 0.82)), character, font=font, fill=255, anchor="ls")
        image = image.resize((width, height), Image.Resampling.LANCZOS)
        offset = len(pixels)
        coverage = [min(15, (value + 8) // 17) for value in image.getdata()]
        if len(coverage) % 2:
            coverage.append(0)
        pixels.extend((coverage[index] << 4) | coverage[index + 1] for index in range(0, len(coverage), 2))
        glyphs.append(f"{{{ord(character)}, {width}, {height}, {advance}, {offset}}}")
    return array(name + "Pixels", pixels, "uint8_t") + f"inline constexpr UiGlyph {name}Glyphs[] = {{\n" + ",\n".join(glyphs) + f"\n}};\ninline constexpr UiFont {name} = {{{name}Pixels, {name}Glyphs, {len(characters)}}};\n"


def main() -> None:
    source = "#pragma once\n#include <stdint.h>\nstruct UiGlyph { uint8_t code, width, height, advance; uint32_t offset; };\nstruct UiFont { const uint8_t *pixels; const UiGlyph *glyphs; uint8_t count; };\n"
    ascii_text = "".join(chr(code) for code in range(32, 127))
    for name, size, characters, filename in [
        ("FONT_SMALL", 11, ascii_text, "seguisb.ttf"),
        ("FONT_BODY", 14, ascii_text, "segoeui.ttf"),
        ("FONT_TITLE", 18, ascii_text, "seguisb.ttf"),
        ("FONT_PET_TIME", 40, "0123456789:-", "segoeuil.ttf"),
        ("FONT_TIME", 58, "0123456789:-", "segoeuil.ttf"),
        ("FONT_VALUE", 45, "0123456789:.-", "segoeuil.ttf"),
    ]:
        source += font_asset(name, size, characters, filename)
    (ROOT / "src/features/ui/ui_assets.h").write_text(source, encoding="utf-8")
    print("UI assets generated")


if __name__ == "__main__":
    main()
