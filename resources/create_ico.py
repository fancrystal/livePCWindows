# Python script to create a valid ICO file
import struct
import zlib
import io
from PIL import Image

# Read the PNG
img = Image.open('D:/work/project/LiveAssistant/resources/logo.png')

# ICO header
header = struct.pack('<HHH', 0, 1, 1)  # reserved, type, count

# Icon directory entry for 256x256 PNG
# Size: 256x256, 32-bit RGBA, PNG format
entry_size = len(open('D:/work/project/LiveAssistant/resources/logo.png', 'rb').read())
entry = struct.pack('<BBBBHHII', 
    0,  # width
    0,  # height (0 means 256)
    0,  # colors
    0,  # reserved
    1,  # planes
    32, # bits per pixel
    entry_size,  # image size
    22  # offset to image data (header + entry = 6 + 16 = 22)
)

# Write ICO file
png_data = open('D:/work/project/LiveAssistant/resources/logo.png', 'rb').read()
with open('D:/work/project/LiveAssistant/resources/logo.ico', 'wb') as f:
    f.write(header)
    f.write(entry)
    f.write(png_data)

print(f"Created ICO with {entry_size} bytes")
