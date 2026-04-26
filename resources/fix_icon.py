import struct
import os

# 读取 PNG 文件
png_path = 'logo.png'
ico_path = 'app_icon.ico'

with open(png_path, 'rb') as f:
    png_data = f.read()

# 获取 PNG 尺寸
if png_data[:8] != b'\x89PNG\r\n\x1a\n':
    print("Not a valid PNG file")
    exit(1)

# 解析 PNG 尺寸
width = struct.unpack('>I', png_data[16:20])[0]
height = struct.unpack('>I', png_data[20:24])[0]
print(f"PNG size: {width}x{height}")

# 创建 ICO 文件
# ICO 文件结构:
# - Header: 6 bytes (reserved, type, count)
# - Directory entry: 16 bytes per image
# - Image data

# 我们只创建一个 256x256 的条目（如果 PNG 是 256x256）
# 或者创建多个标准尺寸

# 对于大于 256 的图片，我们需要调整
if width > 256 or height > 256:
    print(f"Warning: PNG is {width}x{height}, resizing to 256x256")
    # 这里应该使用 PIL 来调整大小，但我们先尝试直接打包

# ICO Header
header = struct.pack('<HHH', 0, 1, 1)  # reserved=0, type=1 (icon), count=1

# ICO Directory Entry
# 对于 256x256，宽度和高度字段为 0
entry_width = width if width < 256 else 0
entry_height = height if height < 256 else 0
entry = struct.pack('<BBBBHHII',
    entry_width,      # width (0 = 256)
    entry_height,     # height (0 = 256)
    0,                # colors (0 = >256)
    0,                # reserved
    1,                # color planes
    32,               # bits per pixel
    len(png_data),    # size of image data
    22                # offset to image data (6 + 16 = 22)
)

# 写入 ICO 文件
with open(ico_path, 'wb') as f:
    f.write(header)
    f.write(entry)
    f.write(png_data)

print(f"Created {ico_path}")
print(f"File size: {os.path.getsize(ico_path)} bytes")

# 验证 ICO 文件头
with open(ico_path, 'rb') as f:
    data = f.read(22)
    reserved, icon_type, count = struct.unpack('<HHH', data[:6])
    print(f"ICO header: reserved={reserved}, type={icon_type}, count={count}")
