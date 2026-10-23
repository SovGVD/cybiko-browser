# CyWIG: the Cybiko Internet Gate protocol, in detail

This is how a Cybiko Xtreme gets to the Internet through a Cybiko Wireless
Internet Gate (CyWIG): its radio and packets, how CyOS finds a gate, how
messages are delivered and acknowledged, and what the WAP browser and the
Browser app send and get back. It covers the Cybiko side only. The gate is
whatever answers on the radio as described here.

Sources: CyOS 1.5.08 (kernel in RAM at 480000-4b4000 once booted), WAP
Browser 21.1.17 for the Xtreme, the Classic's VCyWIG 1.0 and its gateway
boot image (US119.boot), the Cybiko SDK 2.1.13 headers, and packet traces
taken on the radio with these programs running. Things that are inferred
rather than seen are marked as such, and the open questions are listed at
the end.

Conventions: numbers in hex unless they have units. All multi-byte fields
on the radio are big-endian (the H8S's order). "Export N" is CyOS kernel
export number N, as the SDK headers number them (`__cyos N`).

## Contents

1. [Layers](#1-layers)
2. [The radio co-processor](#2-the-radio-co-processor)
3. [Packets](#3-packets)
4. [Cybiko IDs](#4-cybiko-ids)
5. [People around, beacons and finding the gate](#5-people-around-beacons-and-finding-the-gate)
6. [Delivery and acknowledgements](#6-delivery-and-acknowledgements)
7. [Messages](#7-messages)
8. [Messages in several packets](#8-messages-in-several-packets)
9. [Messages in CyOS (the application side)](#9-messages-in-cyos-the-application-side)
10. [Port 93: the gate's services](#10-port-93-the-gates-services)
11. [The WAP exchange](#11-the-wap-exchange)
12. [The Browser exchange](#12-the-browser-exchange)
13. [The Browser page format](#13-the-browser-page-format)
14. [Gate URLs](#14-gate-urls)
15. [Timing and throughput](#15-timing-and-throughput)
16. [Worked packets](#16-worked-packets)
17. [The original gate](#17-the-original-gate)
18. [Open questions](#18-open-questions)

## 1. Layers

```
 application   WAP browser ("WAP")          Browser app ("Browser")
               WML pages                    laid-out pages (section 13)
 ─────────────────────────────────────────────────────────────────────
 service       msg 1022 to port 93 of the gate, replies to the process
 ─────────────────────────────────────────────────────────────────────
 messages      CyOS Message objects: msg id, p0, p1, target, data
               split into packets, acknowledged, numbered
 ─────────────────────────────────────────────────────────────────────
 packets       50 or 200 bytes: 16-byte header, data, Reed-Solomon parity
 ─────────────────────────────────────────────────────────────────────
 radio         AVR co-processor on SCI2, RF chip, channel 2
```

## 2. The radio co-processor

The H8S doesn't drive the RF chip. An AT90S2313 AVR does, and CyOS talks to
it on SCI2 at 36 kbaud, 8N1, feeding it with the DTC. CyOS's radio driver
object (`rfdriver`) is at 49b900-49c760 in RAM.

H8S to AVR:

| Bytes | Meaning |
| --- | --- |
| `01 02 ch` | tune to channel `ch`. CyOS uses 2 |
| `01 03 xx xx` | unknown setting; CyOS sends `00 00` |
| `30 f` + 51 bytes | send a short packet: 50 bytes and one spare byte the DTC count includes. `f` is 0 or 1 (meaning unknown) |
| `cf f` + 201 bytes | send a long packet: 200 bytes and the spare byte |

AVR to H8S:

| Bytes | Meaning |
| --- | --- |
| `03` | the packet went out. CyOS waits up to 50 ms (short) or 100 ms (long) from the start |
| `32` + 50 bytes | a short packet was heard |
| `c8` + 200 bytes | a long packet was heard |
| `13` / `11` | XOFF / XON while CyOS is sending |

A heard packet comes from the AVR at the speed it comes over the air,
about 2400 bytes a second. CyOS expects the last byte within 50 ms (short)
or 115 ms (long) of the first. 15 ms into each packet the driver's TMR1
interrupt (state 4 of the object at `*(4b49fe)`) reads A/D channel 0, which
is the received signal strength. That reading ranks the senders, gates
included (section 5). The AVR doesn't hear while it sends.

## 3. Packets

Two sizes:

| | Total | Data (with header) | Parity | Data after the header |
| --- | --- | --- | --- | --- |
| short | 50 | 30 | 20 | 14 |
| long | 200 | 120 | 80 | 104 |

### Header

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | destination Cybiko ID, `ffffffff` for everyone |
| 4 | 4 | sender's Cybiko ID |
| 8 | 1 | `c0` OR the radio channel (`c2`) |
| 9 | 1 | type and flags (below) |
| a | 1 | part: the packet's place in a message (sections 6, 8) |
| b | 1 | message number (sections 6, 8) |
| c | 2 | CRC-16 of the data after the header (14 or 104 bytes) |
| e | 2 | CRC-16 of bytes 0-d (computed after bytes c-d are filled in) |

Byte 9:

| Bits | Meaning |
| --- | --- |
| e0 | type: `00` beacon, `20` message, `40` acknowledgement, `60` "was it delivered?" |
| 10 | acknowledge this packet |
| 08 | unknown, not seen set |
| 04 | the sender is a CyWIG |
| 02 | unknown, not seen set |
| 01 | (beacons) the beacon carries the owner's details |

Bit 04 matters: see section 5.

### CRC-16

Both CRCs are CRC-16/MODBUS: polynomial 8005 reflected (`a001`), initial
value `ffff`, no final XOR, stored big-endian. In C:

```c
uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t c = 0xffff;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xa001 : c >> 1;
    }
    return c;
}
```

A packet whose CRCs don't match is dropped.

### Reed-Solomon parity

The parity is Reed-Solomon over GF(256) with the field polynomial
x^8+x^4+x^3+x^2+1 (`11d`), primitive element a = 2. These are the tables
in the boot ROM: exp at 170, log at 270, the decoder at 180c. A short
packet has 20 parity bytes and corrects up to 10 bad bytes; a long one has
80 and corrects up to 40.

As a codeword polynomial, parity byte j (packet byte k + j, where k = 30 or
120) is the coefficient of x^j, and data byte i is the coefficient of
x^(n+i), where n is the number of parity bytes. The generator's roots are
a^1 .. a^n:

    g(x) = (x - a^1)(x - a^2) ... (x - a^n)

The parity is the remainder of data(x) * x^n divided by g(x). In C, with
`gen` holding g's coefficients from x^0:

```c
uint8_t r[80] = { 0 };
for (int d = k - 1; d >= 0; d--) {          /* highest coefficient first */
    uint8_t fb = pkt[d] ^ r[n - 1];
    memmove(r + 1, r, n - 1);
    r[0] = 0;
    if (fb) for (int i = 0; i < n; i++) r[i] ^= gf_mul(fb, gen[i]);
}
memcpy(pkt + k, r, n);
```

Packets are sealed in this order: data CRC, header CRC, then parity.

## 4. Cybiko IDs

Every Cybiko has a 32-bit ID, stored in its flash at 7f818. It sits in a
record at 7f800 that starts with the magic `ba0bab15`, and a CRC-32 at
7fffc covers 7f800..7fffb. The boot ROM checks the CRC; a bad one stops
the boot. Xtreme IDs seen start with `4c`.

CyOS drops packets that carry its own ID as the sender, so two Cybikos
with the same ID can't hear each other. Export 701 gives the own ID.

## 5. People around, beacons and finding the gate

Every packet a Cybiko hears, of any type and for anyone, updates its list
of people around. The list is at `G = *(4b4a02)`: 100 entries of 18 bytes.

| Entry offset | Field |
| --- | --- |
| +0 | Cybiko ID |
| +4 | time last heard |
| +e | signal strength (the A/D reading, section 2) |
| +12 | CyWIG flag |

Bit 04 of byte 9 of each packet sets or clears the sender's CyWIG flag.
A gate therefore has to set it on everything it sends, acknowledgements
included. One packet without it and the Cybiko stops taking the sender
for a gate.

The best gate is kept at `G+9fc`: the CyWIG in the list with the highest
signal. Export 1340 (`get_gate_cyid` here) returns its ID, or 0 when there
is none. The WAP browser polls it and shows "Wait until CyWig is found"
until it isn't 0.

### Beacons

A beacon is a packet of type 00 to `ffffffff`. CyOS sends one with flag 01
some seconds after it starts, carrying the owner's details. Its data seen
on a fresh CyOS:

    2a 00 00 00 47 00 6b 6e 6f 77 6e 00 13 00 ...

The nickname or part of it ("known") can be read there. The other fields
are not worked out. A gate beacons to make itself heard: type 00 with flag
04 (byte 9 = `04`), bytes a-b as a running counter, data all zeros. Any
packet with flag 04 does the same job, but beacons keep the gate in the
list while it is quiet; every 3 s is enough.

## 6. Delivery and acknowledgements

A message packet with flag 10 asks for an acknowledgement. The answer is a
short packet of type 40 to the sender that copies bytes a and b. The top 4
bits of its first data byte are the code:

| Code | Meaning |
| --- | --- |
| 0 | taken: the message was handed to its process (message number 0, and each part of a numbered message, section 8) |
| 2 | got it: the packet arrived, delivery to the process is pending |
| 3 | got it again: a repeat of one being worked on |
| 4 | delivered (`DL_SUCCESS`) |
| 5 | nobody took it: no such process or port (`DL_ABORT`) |

CyOS sends each acknowledgement three times, 50-100 ms apart.

There are two ways a sender can go:

- **Message number 0** (byte b = 0): the receiver hands the message over at
  once and answers 0 (or 5). One round trip per packet. Byte a then tells
  the acknowledgements of successive packets apart, so a sender can count
  its packets in it.
- **Other message numbers**: the receiver answers 2. The sender then asks
  with a short packet of type 60 (copying bytes a-b) and gets 4 or 5. Two
  round trips, so about twice as slow.

CyOS uses its own message numbers for what it sends (they go up by one per
message). A sender repeats a packet that wasn't acknowledged. A receiver
has to recognize a repeat (same sender, number and data CRC) and
acknowledge it again without acting twice.

## 7. Messages

The data after the header of a message packet:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | flags (below) |
| 1 | 1 | unknown: 00 in everything traced, 80 seen once |
| 2 | 2 | msg id |
| | 4 | p0, when flag 08 |
| | 4 | p1, when flag 04 |
| | 2 or n | the target: a 16-bit port number, or with flag 20 a NUL-ended process name |
| | 1 | data length, when flag 10 and not 02 |
| | n | the data |

Flags:

| Bit | Meaning |
| --- | --- |
| 20 | the target is a process name, not a port |
| 10 | data follows |
| 08 | p0 follows |
| 04 | p1 follows |
| 02 | the message goes on in more packets (section 8): no length byte, the data runs to the end of the packet |

A port is a number a process listens on; a process name is the name its
`Process` was created with (up to 11 characters). A message to a name the
Cybiko doesn't have is answered with code 5.

With p0, p1, a 7-character process name and the length byte, a long packet
leaves 104 - 21 = 83 bytes of data.

## 8. Messages in several packets

A message too big for one packet goes in parts. This was seen with CyOS
1.5 sending a long URL:

- Byte a of each packet is its part, from 1. Byte b is the number of parts.
  A message in one packet has 0 in byte a and its message number in b.
- Part 1 is the start of the message as in section 7, with flag 02, so its
  data has no length byte and fills the packet.
- The last part's data starts `03 00`, then a length byte and the rest of
  the data.
- Middle parts (inferred): two header bytes and data to the end of the
  packet.
- Every part asks for an acknowledgement and gets one of its own, with code
  0 for parts that aren't the last.

The receiver puts the data of the parts together in order and then reads
the message as one. Trailing zeros of the last packet are padding.

## 9. Messages in CyOS (the application side)

An application sees messages as `struct Message` objects (48 bytes):

| Offset | Field |
| --- | --- |
| 0 | `struct Buffer *buffer`: the data, if any |
| 4 | `char procname[12]` |
| 10 | `struct Message *next` |
| 14 | `char *dst_name`: the target, a process name or a port number below 256 |
| 18 | `cyid_t cyid_from` |
| 1c | `cyid_t cyid_to` |
| 20 | `u8 deleted`, `u8 pad` |
| 22 | `short msgid` |
| 24 | `long param[2]`: p0 and p1 |
| 2c | `void *session` |

A `struct Buffer` is 40 bytes, opaque.

The calls (kernel exports):

| Export | Call |
| --- | --- |
| 270 | `Message_new(size)`: a new message (size = 48) |
| 265 | `Message_delete(m)` |
| 222 | `Buffer_ctor(b, size, increment)` |
| 1196 | `Buffer_store(b, data, offset, length)` |
| 851 | `Buffer_load(b, data, offset, length)` |
| 728 | `Buffer_get_size(b)` |
| 373 | `Message_attach_buffer(m, b)`: the buffer goes with the message and is freed with it |
| 757 | `Message_has_buffer(m)` |
| 640 | `Message_get_buffer(m)` |
| 501 | `Message_deliver(m, target, cyid, timeout)` |
| 413 | `Message_check_delivery(m)`: `DL_NONE` 0, `DL_INQUEUE` 1, `DL_WAIT` 2, `DL_ABORT` 3, `DL_TIMEOUT` 4, `DL_SUCCESS` 5 |
| 943 | `Message_post(m, target, cyid)`: without delivery tracking |
| 687 | `Queue_get_message(queue, timeout, min, max)`: the next message with min <= msgid <= max, or 0 after timeout ms |
| 1340 | the best gate's ID, 0 if none |
| 701 | the own ID |

To send to a port, the target is the port number cast to a pointer:
`Message_deliver(m, (char *)0x93, gate, timeout)`. The timeout is in ms
(the WAP browser uses at most 60000); OR-ing in `MSG_AUTODELETE`
(80000000) makes CyOS delete the message once it is delivered.

A process's queue is at offset 6c of its `Process` object. A message that
comes over the radio to a process name lands in that process's queue with
msgid, p0 and p1 as sent, and the data in an attached buffer.

## 10. Port 93: the gate's services

Both browsers send their requests to port 93 of the gate. CyOS itself also
sends, about every 10 s, a message 47 to port 93 of the best gate, without
asking for an acknowledgement (byte 9 = `20`, message number 0):

    10 00 00 47 00 93 11  ff ff ff 02  4c 80 51 a3  10 22  80 00  43 57 49 47 00

That is flags 10, msg id 47, port 93, 17 bytes of data: `ffffff02`, the
Cybiko's own ID, `1022` (the WAP message id), `8000` and "CWIG". It looks
like a lookup or keep-alive for the gate's msg 1022 service. Neither
browser waits for an answer to it, and a gate that ignores it works.

## 11. The WAP exchange

The WAP browser (process `WAP`):

1. Waits until export 1340 gives a gate.
2. Sends msg 1022 to port 93 of that gate: p0 = 1 (a page; 7 is its other
   request kind, believed to be a post), p1 = its request number, data =
   the URL, NUL-ended. The request number goes up by 10 for each page.
3. Gets the page back as msg 1022 messages to process `WAP`:
   - a part of the page: p0 = `number << 16 | 6`, p1 = `page size << 16 |
     part size`, data = the part. The first part carries the request's
     number and each later one the next number. A part with any other
     number is dropped, and so is a part that comes twice.
   - an error: p0 = `number << 16 | 5`, p1 = an HTTP status. 404 shows
     "Page not found."; others show "error processing URL: %d".
4. Shows the page once it has all of it. A page is at most 5000 bytes
   (checked at offset 1388 of the browser).

While a page comes in, the browser checks every 5 s that the gate is still
the best one, and says "No connection" if not.

The page is WML as text, parsed by the browser itself. It knows `wml`,
`card`, `p`, `br`, `b`, `big`, `small`, `a`, `anchor`, `go` (with
`postfield`), `input` (text and password), `select`, `option`, `timer`,
`onevent` and others. WML variables are `$(name)`, so a literal `$` is
written `$$`. The browser drops the spaces next to tags, so a gate writes
spaces there as `&nbsp;`. Its address field opens with Shift+Tab.

## 12. The Browser exchange

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
bytes of the page format (section 13). A part is packed only when that
makes it smaller, so either form can come. Packed, a part may unpack to
up to 8000 bytes (16000 with pictures) and up to 1200 boxes, and still
takes at most the 2500 (8000) bytes above on the radio; text pages pack
to about a third.

The pages are laid out by the gate for the Cybiko's screen (section 13):
the app doesn't parse HTML or measure text. A long page comes in parts,
each ending with a link to the next one (section 14).

Forms: the app sends a form by going to the submit button's URL with
`?v<var>=<value>&...` for the form's fields:

- text, password, text area: their text
- check box: its value when it is ticked, else empty
- radio button: only the picked one of a group, with its value (all
  buttons of a group carry the group's var)
- list: the value of the chosen option

Values are form-encoded: letters, digits, `-`, `_` and `.` as they are,
space as `+`, everything else as `%XX`. A long form goes in several radio
packets (section 8).

The app's screen: 160x100, a 10-dot title bar, a 90-dot view with a 3-dot
scroll bar at the right, so pages are laid out 156 dots wide.

## 13. The Browser page format

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

## 14. Gate URLs

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

## 15. Timing and throughput

| What | Time |
| --- | --- |
| a short packet on the air | about 21 ms (50 bytes at 2400 B/s) |
| a long packet on the air | about 83 ms |
| a gate's answer part (83-87 bytes) and its acknowledgement | about 300-360 ms |
| page data through the radio | about 240-275 bytes a second |
| a full 5000-byte WAP deck | about 18-20 s |
| a 2500-byte Browser part | about 9-10 s |
| the same packed: about 8000 bytes of text page | about 9-10 s |
| an 8000-byte Browser part with pictures | about 30-40 s, plus the time to fetch them |
| CyOS's msg 47 to the gate | every 10 s |
| gate beacons | every 3 s is enough |

## 16. Worked packets

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

Part 1 of 2. Flags 1e: data running on (02), p0, p1, port target, no
length byte: the URL fills the packet. Part 2 of 2:

    4c80c0de 4c8051a3 c2 30 02 02 0271 2b43
    03 00 08 3d312676353d6200

`03 00`, 8 bytes: `=1&v5=b` and the NUL, the URL's end.

## 17. The original gate

On the Classic, the CyWIG was a Cybiko on a PC's serial port. The PC's
VCyWIG program restarts it with a boot image (US119.boot, an LZSS-packed
mini CyOS 1.2.50 loaded at 200000) that makes it the gate on the radio,
and fetches the pages itself, asking for `text/vnd.wap.wml`,
`application/vnd.wap.wmlc` and `image/vnd.wap.wbmp` (no HTML). It answers
to process `WAP` with msg 1022, in parts of 1024 bytes (128 when it serves
more than one Cybiko). It also relayed CyOS mail over UDP to
mail1.cybiko.com, ports 15170 and 15171; that server is long gone.

## 18. Open questions

- Byte 1 of the message data: 00 in everything traced, 80 seen once.
- Byte 9 bits 08 and 02, and the AVR's `01 03` setting and the `f` byte of
  its send commands.
- What msg 47 to port 93 is for, and whether the original gate answered it.
- The middle parts of a message in three or more packets (only two-part
  messages and the last part were traced).
- The rest of CyOS's beacon data.
- WAP request kind 7: believed to be a post (the URL, a NUL, then the
  form data), not traced.
- `application/vnd.wap.wmlc` (compiled WML) and WBMP pictures: the
  original gate asked for them, so the WAP browser probably shows them, but
  this wasn't tried.
