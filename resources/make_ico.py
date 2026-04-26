from PIL import Image
import sys

try:
    # 打开 PNG 文件
    img = Image.open('logo.png')
    print(f'Original size: {img.size}, mode: {img.mode}')

    # 转换为 RGBA 模式（如果需要）
    if img.mode != 'RGBA':
        img = img.convert('RGBA')

    # 创建多尺寸 ICO 文件
    sizes = [(16,16), (32,32), (48,48), (64,64), (128,128), (256,256)]

    # 保存为 ICO
    img.save('app_icon.ico', format='ICO', sizes=sizes)
    print('Successfully created app_icon.ico')

    # 验证文件
    import os
    if os.path.exists('app_icon.ico'):
        size = os.path.getsize('app_icon.ico')
        print(f'File size: {size} bytes')
    else:
        print('Error: File was not created')
        sys.exit(1)

except Exception as e:
    print(f'Error: {e}')
    import traceback
    traceback.print_exc()
    sys.exit(1)
