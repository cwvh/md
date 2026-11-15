<p align="center">
  <img src="images/logo.png" width="120" alt="md logo">
</p>

# md sample document

A document that exercises every feature of the viewer, for screenshots and
for eyeballing changes. See the [list of features](#features), jump to
[the tables](#tables), open a [second document](linked.md), or visit
[the CommonMark spec](https://spec.commonmark.org/) (hover shows the address).

<!-- this comment is not shown -->

## Features

### Inline formatting

Plain text with *emphasis*, **strong emphasis**, ***both***, ~~strikethrough~~,
`inline code`, and a [link](https://example.com "with a title"). An autolink
<https://example.com/auto> and a bare www.example.org address. A hard break  
follows here, and a second one<br>using HTML. Entities: &copy; &amp; &lt;tag&gt; &#9731;.

A long paragraph, to see how wrapping behaves at different widths: Lorem ipsum
dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut
labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation
ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in
reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur.
Wide characters: 日本語のテキスト、中文文本、한국어. Emoji: 😀 🎉 🚀.

Underscores_in_words stay as they are, and a very long unbreakable token:
aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa

### Headings at every level

# Heading 1
## Heading 2
### Heading 3
#### Heading 4
##### Heading 5
###### Heading 6

---

### Lists

- A bullet list
- With a second item
  - Nested one level
    - And two levels
      - And three, back to the first marker kind
  - Another nested item
- And a third item with a long line that wraps around so that the hanging indent
  of the marker column can be seen at work.

1. Ordered list
2. With numbers
3. In sequence
   1. Nested ordered
   2. Also numbered

A separate list, which starts at seven:

7. A list that starts at seven
8. Continues with eight
9. Nine, then
10. Ten, whose number is wider than the others

- [x] A finished task
- [ ] An open task
- [x] Another done, with `code` and **bold**

Loose list:

- First paragraph item

- Second item, separated by blank lines

  With a second paragraph inside it.

- Third item

### Block quotes

> A quotation that is long enough to wrap onto a second line, in a muted
> color with a bar on its left side.
>
> > A nested quotation, with its own bar.
>
> - And a list
> - inside a quote
>
> ```c
> int in_quote = 1;
> ```

### Code

```c
#include <stdio.h>

/* A comment */
static int fib(int n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

int main(void) {
    printf("fib(10) = %d\n", fib(10));   // prints 55
    return 0;
}
```

```python
def greet(name: str) -> str:
    """Say hello."""
    return f"Hello, {name}!"   # a comment

print(greet("world"), 42, True, None)
```

```json
{
  "name": "md",
  "version": 1,
  "tags": ["markdown", "viewer"],
  "native": true
}
```

```diff
--- a/file.txt
+++ b/file.txt
@@ -1,3 +1,3 @@
 unchanged
-removed line
+added line
```

```
No language: plain monospace text. A line that is far too long to fit in the column, so that it wraps, with its indentation kept: 0123456789 0123456789 0123456789 0123456789 0123456789 0123456789
	A tab-indented line
```

    An indented code block,
    four spaces in.

### Tables

| Left | Center | Right |
|:-----|:------:|------:|
| a    | b      | c     |
| `code` | **bold** | [link](https://example.com) |
| a much longer cell that has plenty of words in it | short | 1234567 |
| ragged | row |

| Name | Description | Notes |
|------|-------------|-------|
| alpha | The first letter of the Greek alphabet, which is also used in the names of many software releases. | early |
| beta | The second letter, and the usual name of a release that is feature complete but not yet well tested. | later |

### Raw HTML

<div align="center">
<b>Centered bold</b> text in an HTML block, with a <a href="https://example.com/html">link</a>,
<code>code</code> and a line break<br>
<img src="images/missing.png" alt="a picture that cannot be loaded" width="200">
</div>

## Last section

The end. This section is here so the document is long enough to scroll and so
that anchor links have somewhere to go. [Back to the top](#md-sample-document).

Another paragraph.

Another paragraph.

Another paragraph.

Another paragraph.

Another paragraph.

Another paragraph.

Another paragraph.

Another paragraph.
