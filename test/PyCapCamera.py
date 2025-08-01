import cv2 as cv
import numpy as np
from urllib.request import urlopen
import os
import datetime
import time
import sys

# 設定 ESP32-CAM 的 IP 位址
url = "http://172.16.18.123/stream"
CAMERA_BUFFER_SIZE = 4096

try:
    print(f"正在連接到攝影機串流：{url}")
    stream = urlopen(url)
    print("連接成功！開始接收影像...")
except Exception as e:
    print(f"連接失敗：{str(e)}")
    print("請確認：")
    print("1. ESP32-CAM 已開機並連接到網路")
    print("2. IP 位址 172.16.18.123 是正確的")
    print("3. ESP32-CAM 的串流服務正在運行")
    sys.exit(1)

bts = b''
i = 0

print("按 'a' 拍照存檔")
print("按 'q' 離開程式")

while True:    
    try:
        bts += stream.read(CAMERA_BUFFER_SIZE)
        
        # 尋找 BMP 檔頭 (BM)
        bmp_head = bts.find(b'BM')
        
        if bmp_head > -1:
            # 如果找到 BMP 檔頭，檢查是否有足夠的數據來讀取檔案大小
            if len(bts) >= bmp_head + 10:
                # BMP 檔案大小位於檔頭後的 2-5 位元組
                file_size_bytes = bts[bmp_head+2:bmp_head+6]
                file_size = int.from_bytes(file_size_bytes, byteorder='little')
                
                # 檢查是否有完整的 BMP 檔案
                if len(bts) >= bmp_head + file_size:
                    # 提取完整的 BMP 檔案
                    bmp_data = bts[bmp_head:bmp_head + file_size]
                    
                    # 移除已處理的數據
                    bts = bts[bmp_head + file_size:]
                    
                    # 將 BMP 數據轉換為 NumPy 陣列
                    img = cv.imdecode(np.frombuffer(bmp_data, dtype=np.uint8), cv.IMREAD_COLOR)
                    
                    if img is not None:
                        # 調整大小並顯示
                        img = cv.resize(img, (640, 480))
                        cv.imshow("ESP32-CAM 串流", img)
        
        k = cv.waitKey(1)
    except Exception as e:
        print(f"錯誤: {str(e)}")
        print("嘗試重新連接...")
        bts = b''
        try:
            stream = urlopen(url)
            print("重新連接成功")
        except Exception as e:
            print(f"重新連接失敗: {str(e)}")
            time.sleep(1)  # 等待一秒後再試
        continue
    
    # 按a拍照存檔
    if k & 0xFF == ord('a'):
        if 'img' in locals() and img is not None:
            # 使用時間戳記作為檔名
            timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"capture_{timestamp}_{i}.jpg"
            cv.imwrite(filename, img)
            print(f"已儲存照片: {filename}")
            i += 1
        else:
            print("尚未接收到有效影像，無法儲存")
    
    # 按q離開
    if k & 0xFF == ord('q'):
        print("程式結束")
        break

cv.destroyAllWindows()