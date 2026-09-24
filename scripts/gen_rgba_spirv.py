# Generate SPIR-V for RGBA passthrough fragment shader (offline, no glslang).
# GLSL:
#   #version 450
#   layout(binding=0) uniform sampler2D uTex;
#   layout(location=0) in vec2 vUV;
#   layout(location=0) out vec4 fragColor;
#   void main() { fragColor = texture(uTex, vUV); }

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
    out = []
    for i in range(0, len(b), 4):
        out.append(int.from_bytes(b[i : i + 4], "little"))
    return out


instr(17, 1)  # OpCapability Shader
instr(11, 1, *str_words("GLSL.std.450"))
instr(14, 0, 1)  # OpMemoryModel Logical GLSL450
instr(15, 4, 4, *str_words("main"), 16, 9)
instr(16, 4, 7)  # OriginUpperLeft
instr(5, 4, *str_words("main"))
instr(5, 9, *str_words("fragColor"))
instr(5, 13, *str_words("uTex"))
instr(5, 16, *str_words("vUV"))
instr(71, 9, 30, 0)  # Location 0 fragColor
instr(71, 13, 34, 0)  # DescriptorSet 0
instr(71, 13, 33, 0)  # Binding 0
instr(71, 16, 30, 0)  # Location 0 vUV
instr(19, 2)  # void
instr(33, 3, 2)  # fn type
instr(22, 6, 32)  # float
instr(23, 7, 6, 4)  # v4
instr(32, 8, 3, 7)  # ptr Output v4
instr(59, 8, 9, 3)  # fragColor
instr(25, 10, 6, 1, 0, 0, 0, 1, 0)  # image
instr(27, 11, 10)  # sampled
instr(32, 12, 0, 11)
instr(59, 12, 13, 0)  # uTex
instr(23, 14, 6, 2)  # v2
instr(32, 15, 1, 14)
instr(59, 15, 16, 1)  # vUV
instr(54, 2, 4, 0, 3)  # OpFunction
instr(248, 5)
instr(61, 11, 17, 13)
instr(61, 14, 18, 16)
instr(87, 7, 19, 17, 18)
instr(62, 9, 19)
instr(253)
instr(56)

words = [0x07230203, 0x00010000, 0x0008000B, 20, 0] + ops
print("count", len(words))
print("const std::vector<uint32_t> kRgbaFragSpirv = {")
line = []
for x in words:
    line.append(f"0x{x:08x}u")
    if len(line) == 8:
        print("    " + ", ".join(line) + ",")
        line = []
if line:
    print("    " + ", ".join(line) + ",")
print("};")
