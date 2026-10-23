# The CyWIG radio and WAP protocol

How the Xtreme's WAP browser talks to a Cybiko Wireless Internet Gate, as
found in CyOS 1.5.08 (its kernel in RAM at 480000-4b4000), WAP Browser
21.1.17 and VCyWIG 1.0 (the PC side of the original gate). `gate/cy_gate.c`
is built on this. Addresses are the kernel's in RAM, or offsets in the WAP
browser's `main.e` image.

## The original gate

On the Classic, `VCyWIG.exe` restarts a Cybiko on the serial port with a
boot image (`US119.boot`: an LZSS-packed mini CyOS 1.2.50 loaded at 200000).
That Cybiko is the gate on the radio; the PC program fetches the pages with
WinInet, asking for `text/vnd.wap.wml`, `application/vnd.wap.wmlc` and
`image/vnd.wap.wbmp`, and refuses HTML. It answers to the process named
`WAP` with msg 1022, in 1024-byte parts (128 when it serves more than one
Cybiko). The gate's other job, relaying CyOS mail over UDP to
mail1.cybiko.com (ports 15170/15171), is dead with the server.

## Packets

The radio co-processor (an AVR) passes 50- or 200-byte packets:

- 30 data + 20 parity bytes, or 120 + 80. Reed-Solomon over GF(256) with the
  polynomial 11d, using the tables in the boot ROM (exp at 170, log at 270,
  decoder at 180c). Parity bytes are the coefficients of x^0..x^(n-1), data
  byte i that of x^(n+i), and the roots are a^1..a^n.
- Header (16 bytes): 0-3 destination (ffffffff for everyone), 4-7 sender,
  8 `c0 | channel` (CyOS uses channel 2), 9 type and flags, 10 the part of a
  message, 11 the message number, 12-13 CRC-16 of the data after the header,
  14-15 CRC-16 of bytes 0-13. Both CRCs are MODBUS (reflected a001, start
  ffff), stored big-endian.
- Byte 9: the top 3 bits are the type, 00 beacon, 20 message, 40
  acknowledgement, 60 "was it delivered?". 10 asks for an acknowledgement;
  04 says the sender is a CyWIG; 01 marks a beacon with the owner's name.

Every packet a Cybiko hears updates its list of people around (`G =
*(4b4a02)`, 100 entries of 18 bytes: +0 ID, +4 time, +e signal, +12 CyWIG
flag), and bit 04 of each packet sets or clears that sender's CyWIG flag, so
a gate has to set it on everything it sends. The best gate (`G+9fc`, read by
kernel export 1340, which the WAP browser polls) is the CyWIG with the
highest signal. The signal is A/D channel 0, read 15 ms into each packet by
the radio driver's TMR1 interrupt (state 4 of the object at `*(4b49fe)`).

## Acknowledgements

A message packet with flag 10 is acknowledged with a short packet of type 40
that copies bytes 10 and 11; the top 4 bits of its first data byte are the
code. CyOS sends three copies.

- Message number (byte 11) 0: the message is handed over at once, code 0.
- Other numbers: code 2 ("got it", or 3 for a repeat being worked on); the
  sender then asks with type 60 and gets 4 (delivered, `DL_SUCCESS`) or 5
  (nobody took it, `DL_ABORT`).

The gate sends with number 0 and counts its packets in byte 10, which
halves the time per part.

## Messages

The data of a message packet (104 bytes in a long packet, 14 in a short one):

| | |
| --- | --- |
| 0 | flags: 08 p0 follows, 04 p1 follows, 20 "to" is a process name (else a 16-bit port), 10 data follows, 02 the message goes on in more packets |
| 1 | 80: (unknown) |
| 2-3 | msg id |
| | p0, p1 (32 bits each, when flagged) |
| | to: a port number, or a NUL-ended process name |
| | data length (1 byte), then the data (with flag 02: no length, the data runs to the end of the packet) |

A message too big for one packet (seen from CyOS 1.5 sending a long URL)
goes in parts: header byte 10 is the part, from 1, and byte 11 how many
there are (a message in one packet has 0 and its message number there).
The first part is the start of the message with flag 02. The last part has
`03 00`, a length byte and the rest of the data; each part is acknowledged
like a message of its own.

## The WAP exchange

The WAP browser waits until export 1340 gives a gate ("Wait until CyWig is
found"), then sends msg 1022 to port 93 of that gate: p0 = 1 (7 for its
other request kind), p1 = its request number (which grows by 10 for each
page), and the URL as data. After msg 47 lookups that need no answer, the
gate replies with msg 1022 messages to the process `WAP`:

- a part of the page: p0 = number << 16 | 6, p1 = page size << 16 | part
  size, the part as data. The first part carries the request's number, and
  each later one the next number; parts with another number are dropped. The
  page is at most 5000 bytes (1388).
- an error: p0 = 5, p1 = an HTTP status (404 shows "Page not found.", others
  "error processing URL: %d").

The page is WML as text (the browser has its own parser: `card`, `p`, `a`,
`anchor`, `go`, `postfield`, `input`, `select`, `option`, `b`, `big`,
`small`, `br`, `timer`, `onevent` and so on). While a page comes in, the
browser checks every 5 seconds that the gate is still the best one, and
says "No connection" if not.

## The Browser app

The Browser app (`cyapp/`) asks the gate the same way, with p0 = 0x21
(+0x100 for the bigger font, +0x200 for the pictures of the part). The gate answers with msg 1022 messages to the
process `Browser`, numbered as above, and the page laid out for the
Cybiko's screen instead of WML: boxes of text in one of CyOS's four fonts
at their places, form fields, rules and picture frames, then a table of
the links and fields. A long page comes in parts of at most 2500 bytes
(about 9 s), each ending with a link to the next part. The format is in
[gate/cy_page.h](../gate/cy_page.h).

