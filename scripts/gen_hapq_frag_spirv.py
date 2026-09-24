# HAP Q YCoCg → RGB fragment SPIR-V
#   vec4 yc = texture(uTex, vUV);
#   float Y=yc.r, Co=yc.g, Cg=yc.b;
#   fragColor = vec4(Y-Co-Cg, Y+Cg, Y+Co-Cg, yc.a);

ops = []


def w(*xs):
    for x in xs:
        ops.append(x & 0xFFFFFFFF)


def instr(opcode, *operands):
    wc = 1 + len(operands)
    w((wc << 16) | opcode, *operands)


def str_words(s):
    b = s.encode("utf-8") + b"\x00"
    while len(b) % 4:
        b += b"\x00"
    return [int.from_bytes(b[i : i + 4], "little") for i in range(0, len(b), 4)]


# IDs 1-19 same as RGBA, then arithmetic 20-29
instr(17, 1)
instr(11, 1, *str_words("GLSL.std.450"))
instr(14, 0, 1)
instr(15, 4, 4, *str_words("main"), 16, 9)
instr(16, 4, 7)
instr(5, 4, *str_words("main"))
instr(5, 9, *str_words("fragColor"))
instr(5, 13, *str_words("uTex"))
instr(5, 16, *str_words("vUV"))
instr(71, 9, 30, 0)
instr(71, 13, 34, 0)
instr(71, 13, 33, 0)
instr(71, 16, 30, 0)
instr(19, 2)
instr(33, 3, 2)
instr(22, 6, 32)
instr(23, 7, 6, 4)
instr(32, 8, 3, 7)
instr(59, 8, 9, 3)
instr(25, 10, 6, 1, 0, 0, 0, 1, 0)
instr(27, 11, 10)
instr(32, 12, 0, 11)
instr(59, 12, 13, 0)
instr(23, 14, 6, 2)
instr(32, 15, 1, 14)
instr(59, 15, 16, 1)

instr(54, 2, 4, 0, 3)
instr(248, 5)
instr(61, 11, 17, 13)  # load sampled
instr(61, 14, 18, 16)  # load uv
instr(87, 7, 19, 17, 18)  # sample → yc
instr(81, 6, 20, 19, 0)  # Y
instr(81, 6, 21, 19, 1)  # Co
instr(81, 6, 22, 19, 2)  # Cg
instr(81, 6, 23, 19, 3)  # A
instr(131, 6, 24, 20, 21)  # Y-Co
instr(131, 6, 25, 24, 22)  # R = Y-Co-Cg
instr(129, 6, 26, 20, 22)  # G = Y+Cg
instr(129, 6, 27, 20, 21)  # Y+Co
instr(131, 6, 28, 27, 22)  # B = Y+Co-Cg
instr(80, 7, 29, 25, 26, 28, 23)  # vec4
instr(62, 9, 29)
instr(253)
instr(56)

words = [0x07230203, 0x00010000, 0x0008000B, 30, 0] + ops
print("count", len(words))
print("const std::vector<uint32_t> kHapQYcocgFragSpirv = {")
line = []
for x in words:
    line.append(f"0x{x:08x}u")
    if len(line) == 8:
        print("    " + ", ".join(line) + ",")
        line = []
if line:
    print("    " + ", ".join(line) + ",")
print("};")
