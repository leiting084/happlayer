# Simple vertex SPIR-V (no gl_PerVertex block):
#   #version 450
#   layout(location=0) in vec2 aPos;
#   layout(location=1) in vec2 aUV;
#   layout(location=0) out vec2 vUV;
#   void main() {
#       gl_Position = vec4(aPos, 0.0, 1.0);
#       vUV = aUV;
#   }

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


# IDs
# 1 GLSL.std.450
# 2 void  3 fn  4 main  5 label
# 6 float  7 v2  8 v4
# 9  ptr_in_v2   10 aPos  11 aUV
# 12 ptr_out_v2  13 vUV
# 14 ptr_out_v4  15 gl_Position
# 16 load aPos
# 17 extract x  18 extract y
# 19 f0  20 f1  21 composite v4
# 22 load aUV
# bound 23

instr(17, 1)  # OpCapability Shader
instr(11, 1, *str_words("GLSL.std.450"))
instr(14, 0, 1)
instr(15, 0, 4, *str_words("main"), 10, 11, 13, 15)  # Vertex
instr(5, 4, *str_words("main"))
instr(5, 10, *str_words("aPos"))
instr(5, 11, *str_words("aUV"))
instr(5, 13, *str_words("vUV"))
instr(5, 15, *str_words("gl_Position"))
instr(71, 10, 30, 0)  # Location 0
instr(71, 11, 30, 1)  # Location 1
instr(71, 13, 30, 0)  # Location 0
instr(71, 15, 11, 0)  # BuiltIn Position

instr(19, 2)  # void
instr(33, 3, 2)
instr(22, 6, 32)  # float
instr(23, 7, 6, 2)  # v2
instr(23, 8, 6, 4)  # v4
instr(32, 9, 1, 7)  # ptr Input v2
instr(59, 9, 10, 1)  # aPos
instr(59, 9, 11, 1)  # aUV
instr(32, 12, 3, 7)  # ptr Output v2
instr(59, 12, 13, 3)  # vUV
instr(32, 14, 3, 8)  # ptr Output v4
instr(59, 14, 15, 3)  # gl_Position
instr(43, 6, 19, 0x00000000)  # OpConstant 0.0
instr(43, 6, 20, 0x3f800000)  # OpConstant 1.0

instr(54, 2, 4, 0, 3)
instr(248, 5)
instr(61, 7, 16, 10)  # load aPos
instr(81, 6, 17, 16, 0)  # CompositeExtract x
instr(81, 6, 18, 16, 1)  # CompositeExtract y
instr(80, 8, 21, 17, 18, 19, 20)  # CompositeConstruct vec4
instr(62, 15, 21)  # store gl_Position
instr(61, 7, 22, 11)  # load aUV
instr(62, 13, 22)  # store vUV
instr(253)
instr(56)

words = [0x07230203, 0x00010000, 0x0008000B, 23, 0] + ops
print("count", len(words), "bound", 23)
print("const std::vector<uint32_t> kSimpleVertSpirv = {")
line = []
for x in words:
    line.append(f"0x{x:08x}u")
    if len(line) == 8:
        print("    " + ", ".join(line) + ",")
        line = []
if line:
    print("    " + ", ".join(line) + ",")
print("};")
