# bytes_pack / bytes_unpack Format String Reference

## Overview

Format strings control how C values are packed into and unpacked from byte
buffers.  The grammar consists of a byte-order prefix, followed by a sequence
of field specifiers.  Two special sub-format families — `$(...)` for strings
and `#(...)` for raw byte slices — handle all variable-length and
fixed-width data fields.

---

## Byte-Order Prefix (optional, must be first character)

| Prefix | Meaning |
|--------|---------|
| `<`    | Little-endian |
| `>`    | Big-endian |
| `=` or `@` | Native endian |

A single byte-order prefix applies to all numeric fields in the format string.
String and byte-slice sub-formats inherit the same byte order for their count
words.

---

## Numeric Field Specifiers

An optional decimal count prefix repeats the specifier N times, consuming N
separate arguments.

| Specifier | Width | C types |
|-----------|-------|---------|
| `b`       | 1 byte | `int8_t` |
| `B`       | 1 byte | `uint8_t` |
| `h`       | 2 bytes | `int16_t` |
| `H`       | 2 bytes | `uint16_t` |
| `i`       | 4 bytes | `int32_t` |
| `I`       | 4 bytes | `uint32_t` |
| `q`       | 8 bytes | `int64_t` |
| `Q`       | 8 bytes | `uint64_t` |
| `f`       | 4 bytes | `float` |
| `d`       | 8 bytes | `double` |

Examples:
- `"<3H"` — three little-endian `uint16_t` values (6 bytes)
- `">IH"` — big-endian `uint32_t` then `uint16_t` (6 bytes)

---

## Padding / Skip: `x`

A decimal count prefix specifies how many bytes to skip or zero-fill.  No
argument is consumed.

| Specifier | Encode | Decode |
|-----------|--------|--------|
| `Nx`      | write N zero bytes | advance offset by N |

Examples:
- `"8x"` — 8 pad/skip bytes
- `">HHI8x"` — EIP socket-address fields (family, port, addr, 8 reserved bytes)

---

## Raw Bytes Insertion: `*` (encode-only)

Inserts the contents of a `Bytes *` argument directly.  No length prefix is
written.  Decode is not supported; use `#(...)` for counted raw-byte fields.

- **Encode arg:** `Bytes *`  (pointer to a `Bytes` struct)
- **Decode:** not supported

Example:
- `"<BB*H"` — two bytes, a raw byte blob, then a uint16

---

## Raw C-String Bytes: `s` (encode-only)

Writes the bytes of a `const char *` argument (no null terminator, no length
prefix).  Decode is not supported; use `$(...)` for strings.

- **Encode arg:** `const char *`
- **Decode:** not supported

---

## String Sub-Format: `$(…)`

Handles all string field variants.  The content between the parentheses is a
small grammar:

```
'$(' [count-word] ['+' field-width] ['z'] ')'

count-word   ::= 'B' | 'H' | 'I'        (1-, 2-, or 4-byte count word)
field-width  ::= '+' decimal             (fixed data-area size in bytes)
'z'                                      (nul byte on the wire)
```

Rules:
- If count-word is present, the count word is written/read first.  It records
  the number of *character* bytes (excluding nul-terminator and padding).
- If `+N` is present, the on-wire data area is always exactly N bytes.  On
  encode the data is zero-padded to N bytes.  On decode the full N bytes are
  consumed but the output slice contains only the count-word-specified length.
- If `z` is present, a nul byte is appended on encode and consumed (skipped)
  on decode.  The decode output does not include the nul byte.
- A leading decimal prefix on the outer `$` (e.g. `16$(...)`) repeats the
  entire string field N times, consuming N argument pairs.

### Encode argument

`const char *`  — nul-terminated C string.  `strlen()` gives the character
count, which is clamped to the maximum representable by the count-word type
(255 for `B`, 65535 for `H`, etc.).

### Decode argument

`Bytes *`  — receives a zero-copy slice into the source buffer.  `.data`
points into the original buffer; `.len` is the character count from the count
word, or field width if there is no count word.  The nul terminator byte (if
`z`) and padding bytes are consumed but are not part of the output slice.

To obtain a nul-terminated C string from a decoded `Bytes`, use
`bytes_to_cstr(Arena *a, Bytes b)` (see below).

### Wire layout

```
[count word]  [character data]  [zero padding to reach +N]  [nul byte if z]
```

### Common CIP string types

| Format  | Wire layout | Notes |
|---------|-------------|-------|
| `$(B)`  | 1-byte count + N bytes | CIP SHORT_STRING; variable length |
| `$(H)`  | 2-byte count + N bytes | Identity product name; variable length |
| `$(H+82)` | 2-byte count + 82-byte data area = 84 bytes | CIP STRING |
| `$(I+82)` | 4-byte count + 82-byte data area = 86 bytes | Logix STRING data area (`2x` for struct padding follows in the main format) |
| `$(z)`  | N bytes + nul byte | C-string with nul delimiter; no count word |
| `$(Bz)` | 1-byte count + N bytes + nul byte | counted and nul-terminated |
| `16$()` | exactly 16 bytes | fixed-width field, no count word; zero-padded on encode |
| `16$(z)` | exactly 16 bytes | fixed-width; decode output trims at first nul |

---

## Raw Byte-Slice Sub-Format: `#(…)`

Identical grammar to `$(...)` but carries binary data instead of text.  No
`z` option.

```
'#(' [count-word] ['+' field-width] ')'

count-word   ::= 'B' | 'H' | 'I'        (1-, 2-, or 4-byte count word)
field-width  ::= '+' decimal             (fixed data-area size in bytes)
```

### Encode argument

`Bytes`  — passed by value.  `.len` is written as the count word (clamped to
the count-word maximum).

### Decode argument

`Bytes *`  — zero-copy slice into the source buffer.

### Common uses

| Format  | Wire layout | Notes |
|---------|-------------|-------|
| `#(B)`  | 1-byte count + N bytes | small opaque payload |
| `#(H)`  | 2-byte count + N bytes | medium opaque payload |
| `#(I)`  | 4-byte count + N bytes | large opaque payload |
| `16#()` | exactly 16 bytes | fixed-width raw slice, no count word |

---

## `bytes_to_cstr`

```c
char *bytes_to_cstr(Arena *a, Bytes b);
```

Allocates `b.len + 1` bytes in the arena, copies `b.data`, appends a nul
byte, and returns a `char *`.  Use when a decoded `Bytes` must be passed to a
function that requires a nul-terminated C string.

---

## Complete Format String Grammar (EBNF)

```
format      ::= [byte_order] field*
byte_order  ::= '<' | '>' | '=' | '@'
field       ::= count? specifier
count       ::= [0-9]+
specifier   ::= numeric | 'x' | 's' | '*' | string_sub | bytes_sub
numeric     ::= 'b'|'B'|'h'|'H'|'i'|'I'|'q'|'Q'|'f'|'d'
string_sub  ::= '$(' count_word? ('+' [0-9]+)? 'z'? ')'
bytes_sub   ::= '#(' count_word? ('+' [0-9]+)?       ')'
count_word  ::= 'B' | 'H' | 'I'
```

---

## Full Examples

```c
// EIP header: cmd(2) len(2) session(4) status(4) context(8) options(4)
bytes_pack(a, "<HHIIQI", cmd, len, session, status, context, options);

// EIP socket address (big-endian): family port addr 8-reserved
bytes_pack(a, ">HHI8x", sin_family, sin_port, sin_addr);

// CIP identity fields + SHORT_STRING product name
//   vendor(2) dev_type(2) prod_code(2) rev_major(1) rev_minor(1)
//   status(2) serial(4) name(variable, 1-byte count)
bytes_pack(a, "<HHHBBHI$(B)", vendor, dev_type, prod_code,
              rev_major, rev_minor, status, serial, product_name);

// Logix STRING (4-byte count + 82-byte data + 2-byte padding)
bytes_pack(a, "<$(I+82)2x", logix_string);

// Decode: EIP header then CPF item count
uint16_t cmd, len, item_count;
uint32_t session, status;
uint64_t context;
uint32_t options;
Bytes rest = bytes_unpack(data, "<HHIIQI", &cmd, &len, &session,
                          &status, &context, &options);
rest = bytes_unpack(rest, "<H", &item_count);

// Decode: sockaddr + identity fields + SHORT_STRING name
uint16_t sin_family, sin_port;
uint32_t sin_addr;
uint16_t vendor_id, dev_type, prod_code, status_word;
uint8_t rev_major, rev_minor;
uint32_t serial;
Bytes product_name = {0};
identity = bytes_unpack(identity, ">HHI8x<HHHBBHI$(B)",
                        &sin_family, &sin_port, &sin_addr,
                        &vendor_id, &dev_type, &prod_code,
                        &rev_major, &rev_minor, &status_word, &serial,
                        &product_name);
printf("  Product Name: %.*s\n", (int)product_name.len, product_name.data);

// Or, if a C string is needed elsewhere:
char *name_cstr = bytes_to_cstr(a, product_name);
printf("  Product Name: %s\n", name_cstr);
```
