# The Browser protocol

What the Browser app (`cyapp/`) and the gate (`gate/`) say to each other:
the request, the answer, the page format the gate lays out for the Cybiko's
screen, and the gate's short URLs. This is this project's own use of the
CyWIG service; everything underneath it is CyOS's and is documented in
[cybiko-docs](https://github.com/SovGVD/cybiko-docs):

- [radio-protocol.md](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md): packets, CRCs and
  parity, Cybiko IDs, delivery and acknowledgements, messages and messages
  in parts;
- [cywig.md](https://github.com/SovGVD/cybiko-docs/blob/main/cywig.md): how CyOS finds a gate (the gate flag,
  pings, the best gate), port 93, packed messages, the Xtreme WAP browser's
  requests and the WML it reads, timing, the original VCyWIG gate;
- [cyos.md](https://github.com/SovGVD/cybiko-docs/blob/main/cyos.md#messages-and-processes): messages in CyOS
  and the kernel exports the app uses.

Conventions: numbers in hex unless they have units; all multi-byte fields
are big-endian.

## Contents

1. [The Browser exchange](#1-the-browser-exchange)
2. [The Browser page format](#2-the-browser-page-format)
3. [Gate URLs](#3-gate-urls)
4. [Timing](#4-timing)
5. [Worked packets](#5-worked-packets)

## 1. The Browser exchange

The Browser app (process `Browser`) speaks the same service with its own
request kind and its own answer format.

Request: msg 1022 to port 93 of the gate, data = the URL, NUL-ended.

| Field | Value |
| --- | --- |
| p0 | `21`, plus `100` for the bigger fonts, plus `200` for the pictures of the part, plus `400` if the app unpacks packed parts |
| p1 | the request number |

The request number must be past every number the last answer used (the
app uses the last part's number plus one).

Answer: msg 1022 messages to process `Browser`, framed exactly as for WAP:
p0 = `number << 16 | 6`, p1 = `total << 16 | part size`, the parts in
order, numbered from the request's number. The total is at most 2500 bytes
(8000 with pictures). Errors come as pages, not as p0 = 5.

Packed parts: when p0 has `400`, the gate may send a part packed. Then
what comes in is `43 5a` ("CZ"), a 16-bit length, and the part in raw
Deflate (RFC 1951, no zlib header), which unpacks to exactly that many
bytes of the page format (section 2). A part is packed only when that
makes it smaller, so either form can come. Packed, a part may unpack to
up to 8000 bytes (16000 with pictures) and up to 1200 boxes, and still
takes at most the 2500 (8000) bytes above on the radio; text pages pack
to about a third.

The pages are laid out by the gate for the Cybiko's screen (section 2):
the app doesn't parse HTML or measure text. A long page comes in parts,
each ending with a link to the next one (section 3).

Forms: the app sends a form by going to the submit button's URL with
`?v<var>=<value>&...` for the form's fields:

- text, password, text area: their text
- check box: its value when it is ticked, else empty
- radio button: only the picked one of a group, with its value (all
  buttons of a group carry the group's var)
- list: the value of the chosen option

Values are form-encoded: letters, digits, `-`, `_` and `.` as they are,
space as `+`, everything else as `%XX`. A long form goes in several radio
packets ([messages in parts](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md#messages-in-parts)).

The app's screen: 160x100, a 10-dot title bar, a 90-dot view with a 3-dot
scroll bar at the right, so pages are laid out 156 dots wide.

## 2. The Browser page format

All numbers big-endian.

### Header

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 3 | `43 42 01`: "CB" and version 1 |
| 3 | 1 | flags: 01 a next part follows (its URL is target 1) |
| 4 | 2 | height of this part in dots |
| 6 | 2 | offset of the target table |
| 8 | 1 + n | the title: length, then Latin-1 text (at most 40) |

### Boxes

From the end of the title up to the target table, box records, in reading
order:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | kind << 4 OR font |
| 1 | 1 | x, from the left of the page |
| 2 | 2 | y, from the top of this part |
| 4 | 1 | width |
| 5 | 1 | height |
| 6 | 1 | style |
| 7 | 1 | target number, 0 none |
| 8 | | kind-specific (below) |

Kinds:

| Kind | Name | After the record |
| --- | --- | --- |
| 0 | text | length byte and Latin-1 text |
| 1 | picture frame (picture not sent) | nothing |
| 2 | form field (its state is in its target) | nothing |
| 3 | rule (drawn 2 dots down) | nothing |
| 4 | picture's alternative text, already in brackets | length byte and text |
| 5 | picture | height rows of (width + 3) / 4 bytes: 2 bits a pixel, the leftmost in the top bits, 0 white, 1 light gray, 2 dark gray, 3 black |

Fonts are CyOS's own: 0 mini (9 dots high), 1 mini bold, 2 cool (12 dots,
the desktop's), 3 cool bold. Text boxes are drawn at their x and y with the
font's own glyph placement (the layout was measured with the same fonts).

Styles (bits): 01 bold, 02 big (heading), 04 preformatted, 08 link
(underline it), 10 centred.

A box with a target is part of a link or a field. The focused target is
shown inverted (text, pictures) or with a second frame (fields).

### Target table

| Size | Field |
| --- | --- |
| 1 | count |
| | the targets, numbered from 1 |

Target 1 is always the next part's link, empty on the last part. Each
target starts with its kind:

**0, a link**: length byte, then the URL.

**1, a form field**:

| Size | Field |
| --- | --- |
| 1 | field type: 0 text, 1 password, 3 submit, 4 check box, 5 radio button, 6 list, 7 text area, 8 reset |
| 1 | form number (ff: none) |
| 1 | flags: 01 checked |
| 2 | var: the number its value is sent as (`v<var>`) |
| 1 + n | value: the text in it, the check box's or radio button's value, or a button's caption (at most 80) |
| | lists only: 1 option count, 1 chosen option, then per option a value and a label (length byte and text each) |
| | submit buttons only: length byte and the URL the form goes to |

A target table too big for the part has its last targets as empty links,
so the numbers still hold.

### An example

The start of a short test page:

    43 42 01  00  01 3e  .. ..  12 "Text test - página"
    03 00 00 00 2b 0c 03 00  0b "Heading one"
    00 00 00 12 41 09 00 00  11 "A paragraph with "
    01 41 00 12 10 09 01 00  04 "bold"
    ...

"CB" version 1, no next part, 318 dots high, the target table's offset,
an 18-byte title. Then a text box in cool bold (kind 0, font 3) at 0,0,
43x12, style 03 (bold, big); a mini text box at 0,18, 65x9; and a mini
bold one right after it at 65,18, 16x9, style 01.

## 3. Gate URLs

Pages from the gate carry short URLs of its own under `http://c/`, which
keeps what goes over the radio small. They are only good on the gate that
gave them out, and only for a while (the gate keeps the last 512).

| URL | Meaning |
| --- | --- |
| `http://c/` | the gate's home page (also `http://www.cybiko.com/wap/index.asp`, the WAP browser's Home) |
| `http://c/N` | a link whose URL was too long to send |
| `http://c/fN?v<var>=...` | a form: N stands for its action, method and hidden fields |
| `http://c/pN` | the next WML deck of a page (WAP) |
| `http://c/yN` | the next part of a laid-out page (Browser) |
| `http://c/wifi`, `?scan` | the gate's Wi-Fi page, and with a new scan |
| `http://c/jN[?p=password]` | join network N (without `p`: ask for the password) |
| `http://c/xN` | forget saved network N |
| `http://c/cookies`, `?clear` | how many cookies the gate keeps for this Cybiko, and dropping them |

The gate keeps the web's cookies for each Cybiko by its ID (for up to 8
Cybikos, until the gate restarts), so a Cybiko stays logged in to a site
across requests, both browsers alike.

An expired short URL answers with a page saying so. When the gate has no Internet
it answers any request with its Wi-Fi page instead, with the reason on top.

## 4. Timing

On top of the radio timing in [cywig.md](https://github.com/SovGVD/cybiko-docs/blob/main/cywig.md#5-timing):

| What | Time |
| --- | --- |
| a 2500-byte Browser part | about 9-10 s |
| the same packed: about 8000 bytes of text page | about 9-10 s |
| an 8000-byte Browser part with pictures | about 30-40 s, plus the time to fetch them |

## 5. Worked packets

From a trace with Cybiko `4c8051a3` and gate `4c80c0de` (data part only;
the parity follows).

**The Browser app asks for the home page** (long packet):

    4c80c0de 4c8051a3 c2 30 00 01 e187 e57b
    1c 00 1022 00000021 00000001 0093 0a 687474703a2f2f632f00

To the gate, channel 2, a message asking for an acknowledgement, single
packet, message number 1. Flags 1c: data, p0, p1, port target. Msg 1022,
p0 = 21 (a Browser page), p1 = request 1, port 93, 10 bytes of data:
`http://c/` and its NUL.

**The gate's acknowledgement** (short packet):

    4c8051a3 4c80c0de c2 44 00 01 .... ....
    40

Type 40 with the CyWIG flag 04; bytes a-b copied; code 4, delivered.

**The first part of the answer** (long packet):

    4c8051a3 4c80c0de c2 34 01 00 .... ....
    3c 00 1022 00010006 03940053 42726f7773657200 53 434201 ...

Message with acknowledgement and the CyWIG flag. Part 1, message number 0.
Flags 3c: data, p0, p1, process name. p0 = `0001 0006` (number 1, a page
part), p1 = `0394 0053` (916 bytes in all, 83 here), to "Browser", 83
bytes of page.

**The Cybiko's acknowledgement of it** (short packet, sent 3 times):

    4c80c0de 4c8051a3 c2 40 01 00 01ab 0f22
    00

Type 40, bytes a-b copied (part 1, number 0), code 0: handed over.

**A form too long for one packet**, part 1 of 2:

    4c80c0de 4c8051a3 c2 30 01 02 bb6c f6f0
    1e 00 1022 00000021 0000000e 0093 687474703a2f2f632f66333f76303d...

Part 1, message number 2 (byte b is the message number, not the part count; see [radio-protocol.md](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md#11-earlier-readings)). Flags 1e: data running on (02), p0, p1, port target, no
length byte: the URL fills the packet. Part 2, the last:

    4c80c0de 4c8051a3 c2 30 02 02 0271 2b43
    03 00 08 3d312676353d6200

`03 00`, 8 bytes: `=1&v5=b` and the NUL, the URL's end.

