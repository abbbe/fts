#!/usr/bin/env python3
"""
Extract PPTX content to Markdown while preserving bullet point structure and formatting.

Formatting annotations:
- Text color: [RED: text], [BLUE: text], etc.
- Background/highlight: [@YELLOW: text], [@GREEN: text], etc.
- Combined: [RED: [@YELLOW: emphasized text]]
- Bold: **text**, Italic: *text*

Usage:
    python pptx_to_markdown.py presentation.pptx
    python pptx_to_markdown.py presentation.pptx > output.md
"""

import argparse
import os
import sys
import tempfile
import zipfile
import xml.etree.ElementTree as ET

# XML namespaces used in PPTX files
NS = {
    'a': 'http://schemas.openxmlformats.org/drawingml/2006/main',
    'p': 'http://schemas.openxmlformats.org/presentationml/2006/main',
    'r': 'http://schemas.openxmlformats.org/officeDocument/2006/relationships'
}

# Map RGB hex values to color names
# Using approximate matching for common colors
# Note: BLACK and WHITE are excluded as they're typically default text/background
COLOR_MAP = {
    # Reds
    'FF0000': 'RED', 'CC0000': 'RED', 'C00000': 'RED', 'FF3333': 'RED',
    # Greens
    '00FF00': 'GREEN', '00CC00': 'GREEN', '008000': 'GREEN', '00B050': 'GREEN',
    '92D050': 'LIME',
    # Blues
    '0000FF': 'BLUE', '0000CC': 'BLUE', '0070C0': 'BLUE', '00B0F0': 'CYAN',
    '5B9BD5': 'BLUE',
    # Yellows
    'FFFF00': 'YELLOW', 'FFC000': 'ORANGE', 'FFD700': 'GOLD',
    # Purples/Magentas
    'FF00FF': 'MAGENTA', '7030A0': 'PURPLE', '9933FF': 'PURPLE',
    # Gray (for emphasis on light backgrounds)
    '808080': 'GRAY',
}


def rgb_to_color_name(hex_val):
    """
    Convert RGB hex value to color name.
    Returns None for black (default text color) or unrecognized colors.
    """
    hex_val = hex_val.upper()

    # Direct match
    if hex_val in COLOR_MAP:
        return COLOR_MAP[hex_val]

    # Skip black/white (usually default text/background)
    if hex_val in ('000000', '000001', 'FFFFFF', 'FEFEFE'):
        return None

    # Try to classify by dominant channel
    try:
        r = int(hex_val[0:2], 16)
        g = int(hex_val[2:4], 16)
        b = int(hex_val[4:6], 16)

        # High saturation colors only
        max_c = max(r, g, b)
        min_c = min(r, g, b)

        if max_c - min_c < 50:  # Low saturation = gray/black/white
            return None

        if r > 200 and g < 100 and b < 100:
            return 'RED'
        if g > 200 and r < 100 and b < 100:
            return 'GREEN'
        if b > 200 and r < 100 and g < 100:
            return 'BLUE'
        if r > 200 and g > 200 and b < 100:
            return 'YELLOW'
        if r > 200 and g > 100 and b < 100:
            return 'ORANGE'
        if r > 200 and b > 200 and g < 100:
            return 'MAGENTA'
        if b > 200 and g > 200 and r < 100:
            return 'CYAN'

    except (ValueError, IndexError):
        pass

    return None


def extract_run_formatting(rPr):
    """
    Extract formatting from run properties element.

    Returns:
        tuple: (is_bold, is_italic, text_color, highlight_color)
    """
    if rPr is None:
        return False, False, None, None

    is_bold = rPr.get('b') == '1'
    is_italic = rPr.get('i') == '1'

    text_color = None
    highlight_color = None

    # Text color from solidFill
    solidFill = rPr.find('a:solidFill', NS)
    if solidFill is not None:
        srgbClr = solidFill.find('a:srgbClr', NS)
        if srgbClr is not None:
            text_color = rgb_to_color_name(srgbClr.get('val', ''))

    # Highlight/background color
    highlight = rPr.find('a:highlight', NS)
    if highlight is not None:
        srgbClr = highlight.find('a:srgbClr', NS)
        if srgbClr is not None:
            highlight_color = rgb_to_color_name(srgbClr.get('val', ''))

    return is_bold, is_italic, text_color, highlight_color


def format_text_with_annotations(text, is_bold, is_italic, text_color, highlight_color):
    """
    Apply formatting annotations to text.
    """
    if not text:
        return text

    result = text

    # Apply bold/italic (innermost)
    if is_bold and is_italic:
        result = f"***{result}***"
    elif is_bold:
        result = f"**{result}**"
    elif is_italic:
        result = f"*{result}*"

    # Apply highlight (middle layer)
    if highlight_color:
        result = f"[@{highlight_color}: {result}]"

    # Apply text color (outermost)
    if text_color:
        result = f"[{text_color}: {result}]"

    return result


def extract_slide_content(slide_path):
    """
    Extract text from a single slide preserving bullet point structure and formatting.

    Args:
        slide_path: Path to slide XML file

    Returns:
        List of (level, text) tuples where level indicates indentation
    """
    tree = ET.parse(slide_path)
    root = tree.getroot()

    content = []

    # Find all shape elements (text boxes, titles, etc.)
    for sp in root.findall('.//p:sp', NS):
        txBody = sp.find('.//p:txBody', NS)
        if txBody is None:
            continue

        for para in txBody.findall('a:p', NS):
            # Get paragraph properties for indentation level
            pPr = para.find('a:pPr', NS)
            level = 0
            if pPr is not None:
                level = int(pPr.get('lvl', '0'))

            # Process each text run with its formatting
            para_text_parts = []

            for run in para.findall('a:r', NS):
                rPr = run.find('a:rPr', NS)
                t = run.find('a:t', NS)

                if t is not None and t.text:
                    is_bold, is_italic, text_color, highlight_color = extract_run_formatting(rPr)
                    formatted = format_text_with_annotations(
                        t.text, is_bold, is_italic, text_color, highlight_color
                    )
                    para_text_parts.append(formatted)

            # Also check for direct text elements (not in runs)
            for t in para.findall('a:t', NS):
                # Skip if this t is inside an a:r (already processed)
                parent = t.getparent() if hasattr(t, 'getparent') else None
                # ElementTree doesn't have getparent, so we check differently
                pass  # Already handled by finding direct children

            text = ''.join(para_text_parts).strip()
            if text:
                content.append((level, text))

    return content


def format_slide_as_markdown(slide_num, content):
    """
    Format slide content as Markdown.

    Args:
        slide_num: Slide number for header
        content: List of (level, text) tuples

    Returns:
        Markdown formatted string
    """
    lines = [f"# Slide {slide_num}"]

    for level, text in content:
        if level == 0:
            # Top-level items: could be title or main bullet
            # Short items are likely titles (unless they have formatting annotations)
            is_title = len(text) < 60 and not any(c in text for c in '.!?')
            # But if it has color annotations, might be longer
            plain_text = text
            for prefix in ['[RED:', '[BLUE:', '[GREEN:', '[YELLOW:', '[@']:
                plain_text = plain_text.replace(prefix, '')
            is_title = len(plain_text) < 80 and not any(c in plain_text for c in '.!?')

            if is_title:
                lines.append(f"\n## {text}")
            else:
                lines.append(f"\n{text}")
        else:
            # Indented bullet points
            indent = '  ' * (level - 1)
            lines.append(f"{indent}- {text}")

    return '\n'.join(lines)


def extract_pptx(pptx_path):
    """
    Extract all slides from PPTX file to Markdown.

    Args:
        pptx_path: Path to PPTX file

    Returns:
        Markdown formatted string with all slides
    """
    if not os.path.exists(pptx_path):
        raise FileNotFoundError(f"File not found: {pptx_path}")

    with tempfile.TemporaryDirectory() as tmpdir:
        # Extract PPTX (it's a ZIP archive)
        with zipfile.ZipFile(pptx_path, 'r') as z:
            z.extractall(tmpdir)

        slides_dir = os.path.join(tmpdir, 'ppt', 'slides')
        if not os.path.exists(slides_dir):
            raise ValueError(f"Invalid PPTX file: no slides directory found")

        # Get all slide files sorted by number
        slide_files = [
            f for f in os.listdir(slides_dir)
            if f.startswith('slide') and f.endswith('.xml')
        ]
        slide_files.sort(key=lambda x: int(x.replace('slide', '').replace('.xml', '')))

        # Process each slide
        all_slides = []
        for slide_file in slide_files:
            slide_num = slide_file.replace('slide', '').replace('.xml', '')
            slide_path = os.path.join(slides_dir, slide_file)

            content = extract_slide_content(slide_path)
            if content:
                markdown = format_slide_as_markdown(slide_num, content)
                all_slides.append(markdown)

        return '\n\n---\n\n'.join(all_slides)


def main():
    parser = argparse.ArgumentParser(
        description='Extract PPTX content to Markdown preserving bullet structure and formatting'
    )
    parser.add_argument('pptx_file', help='Path to PPTX file')
    parser.add_argument('-o', '--output', help='Output file (default: stdout)')

    args = parser.parse_args()

    try:
        markdown = extract_pptx(args.pptx_file)

        if args.output:
            with open(args.output, 'w', encoding='utf-8') as f:
                f.write(markdown)
            print(f"Written to {args.output}", file=sys.stderr)
        else:
            print(markdown)

    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
