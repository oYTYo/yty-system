# 文件名: run_uniq_service.py
import zmq
import torch
import joblib
import pandas as pd
import numpy as np
import json
import os
import time
from collections import deque

# --- 1. 模型定义 (必须与 train_uniq.py 保持一致) ---
import torch.nn as nn

class UniQModel(nn.Module):
    def __init__(self):
        super(UniQModel, self).__init__()
        # Branch A: 物理参数预测 alpha, beta
        self.branch_a = nn.Sequential(
            nn.Linear(7, 64), nn.ReLU(),
            nn.Linear(64, 32), nn.ReLU(),
            nn.Linear(32, 2)
        )
        # Branch B: 网络参数预测 Penalty
        self.branch_b = nn.Sequential(
            nn.Linear(10, 64), nn.ReLU(),
            nn.Linear(64, 32), nn.ReLU(),
            nn.Linear(32, 1), nn.ReLU()
        )

    def forward(self, inputs):
        bitrate = inputs[:, 0:1]
        feat_a = torch.cat([inputs[:, 7:11], inputs[:, 1:4]], dim=1)
        ab = self.branch_a(feat_a)
        alpha = ab[:, 0:1]
        beta = ab[:, 1:2]
        
        rho_effective = alpha * bitrate + beta
        # 返回计算 k 所需的 raw outputs
        return alpha, rho_effective

# --- 2. 配置与加载 ---
MODEL_PATH = "scratch/UniQ/uniq_100_std.pth"  # 确保路径正确
SCALER_PATH = "scratch/UniQ/scaler.pkl"              # 确保路径正确

device = torch.device("cpu") # 推理通常 CPU 够用且延迟低
print(f"Loading model from {MODEL_PATH}...")
model = UniQModel().to(device)
model.load_state_dict(torch.load(MODEL_PATH, map_location=device))
model.eval()

print(f"Loading scaler from {SCALER_PATH}...")
scaler = joblib.load(SCALER_PATH)

# --- 3. 全局状态 (用于归一化 k 值) ---
# 存储每个摄像头最新的 k 值: {camera_id: k_value}
camera_k_values = {}
# 设定的 k 值过期时间，防止离线的摄像头影响均值 (例如 2.0秒)
camera_last_update = {} 

def calculate_k(features_tensor):
    with torch.no_grad():
        alpha, rho = model(features_tensor)
        sig_rho = torch.sigmoid(rho)
        # k = 100 * sigmoid(rho) * (1 - sigmoid(rho)) * alpha
        k_val = 100 * sig_rho * (1 - sig_rho) * alpha
    return k_val.item()

def preprocess_data(data):
    # 解析分辨率
    res_str = data['Resolution'] # e.g. "1920x1080"
    if 'x' in res_str:
        w, h = map(int, res_str.split('x'))
    else:
        w, h = 0, 0 # 异常处理
    pixels = w * h
    
    # 构造 Codec One-Hot
    codec = data['Codec']
    codec_h264 = 1 if codec == 'H.264' else 0
    codec_h265 = 1 if codec == 'H.265' else 0
    codec_vp9  = 1 if codec == 'VP9' else 0
    codec_av1  = 1 if codec == 'AV1' else 0
    
    # 构造 DataFrame 以匹配 scaler 的输入特征顺序
    # ['AvgActualBitrate(kbps)', 'Pixels', 'Encoding_fps', 'AvgCRF', 
    #  'Throughput(kbps)', 'AvgDelay(ms)', 'AvgLossRate']
    raw_features = pd.DataFrame([{
        'AvgActualBitrate(kbps)': data['AvgActualBitrate(kbps)'],
        'Pixels': pixels,
        'Encoding_fps': data['Encoding_fps'],
        'AvgCRF': data['AvgCRF'],
        'Throughput(kbps)': data['Throughput(kbps)'],
        'AvgDelay(ms)': data['AvgDelay(ms)'],
        'AvgLossRate': data['AvgLossRate']
    }])
    
    # 归一化
    scaled_features = scaler.transform(raw_features)
    
    # 拼接 Codec 特征 (Codec 不需要归一化)
    # 输入顺序必须与模型 forward 中的切片一致:
    # 0: Bitrate, 1: Pixels, 2: FPS, 3: CRF, 4: Throughput, 5: Delay, 6: Loss
    # 7-10: Codec (H.264, H.265, VP9, AV1)
    
    final_input = np.hstack([
        scaled_features, 
        [[codec_h264, codec_h265, codec_vp9, codec_av1]]
    ])
    
    return torch.tensor(final_input, dtype=torch.float32).to(device)

# --- 4. ZMQ 服务循环 ---
def run_server():
    context = zmq.Context()
    socket = context.socket(zmq.REP)
    socket.bind("tcp://*:5556")
    
    print("UniQ Inference Server Started on tcp://*:5556")
    
    while True:
        # 接收请求
        message = socket.recv_string()
        request = json.loads(message)
        
        cam_id = request['cameraId']
        current_time = time.time()
        
        try:
            # 1. 预处理 & 推理
            input_tensor = preprocess_data(request)
            k_value = calculate_k(input_tensor)
            
            # 2. 更新全局状态
            camera_k_values[cam_id] = k_value
            camera_last_update[cam_id] = current_time
            
            # 3. 清理过期的摄像头数据 (超过2秒没更新的)
            active_cams = [cid for cid, t in camera_last_update.items() if current_time - t < 2.0]
            
            # 4. 计算归一化均值 (Mean of active cameras)
            # 如果只有一个摄像头，均值就是它自己，权重为1
            current_k_values = [camera_k_values[cid] for cid in active_cams]
            
            if len(current_k_values) > 0:
                avg_k = sum(current_k_values) / len(current_k_values)
            else:
                avg_k = k_value # Fallback
            
            # 避免除以零
            if avg_k < 1e-6:
                weight = 1.0
            else:
                weight = k_value / avg_k
                # weight = ( k_value / avg_k )**2

            # codec_adjustment = 0.5 if request['Codec'] == 'H.264' else (-0.5 if request['Codec'] == 'AV1' else 0)
            codec_adjustment = -0.5 if request['Codec'] == 'AV1' else 0
            weight = weight + codec_adjustment
            
            # 5. 范围限制 [0.5, 2.0]
            weight = max(0.5, min(weight, 2.0))
            
            # print(f"Cam {cam_id}: k={k_value:.4f}, avg_k={avg_k:.4f}, w={weight:.4f}")
            
            # 发送回复
            socket.send_json({"targetWeight": weight})
            
        except Exception as e:
            print(f"Error processing cam {cam_id}: {e}")
            socket.send_json({"targetWeight": 1.0})

if __name__ == "__main__":
    run_server()