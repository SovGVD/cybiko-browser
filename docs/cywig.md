# The gate

`gate/cy_gate.c` is a Cybiko Wireless Internet Gate (CyWIG) for the
Xtreme's WAP browser and for Browser. How a Cybiko finds a gate and what it
sends to it is CyOS's and is documented in cybiko-docs:
[cywig.md](https://github.com/SovGVD/cybiko-docs/blob/main/cywig.md) (the
gate flag, port 93, packed messages, the WAP exchange, the original VCyWIG
gate) and
[radio-protocol.md](https://github.com/SovGVD/cybiko-docs/blob/main/radio-protocol.md)
(packets, delivery, messages). What Browser asks and gets back is in
[cywig-spec.md](cywig-spec.md).

What this gate does on top of that:

- It sets the gate flag (04 in byte 9) on every packet it sends,
  acknowledgements included, and pings every 3 s with it.
- It sends its answer parts with message number 0 and counts its packets in
  byte a, so each packet is handed over at once with one acknowledgement:
  about half the time per part of the numbered way.
- It answers the WAP browser with WML decks of at most 5000 bytes (the
  browser's limit) and Browser with laid-out pages, and keeps long links and
  forms behind its own short URLs (`http://c/...`).
- It logs CyOS's packed messages to "CWIG" and doesn't answer them; both
  browsers work without an answer.
