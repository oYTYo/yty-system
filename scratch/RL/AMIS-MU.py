import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
from torch.distributions import Categorical
import numpy as np
import zmq
import json
import os
import collections
import time
import math
import sys
import random

# ==========================================
# --- 0. 全局控制开关 (双旋钮配置) ---
# ==========================================

# [旋钮 1] 训练模式
TRAIN_MODE = False

# [旋钮 2] 推理学习模式 (Inference Learning)
INFERENCE_LEARNING = True

# [超参数] 推理模式下的探索温度系数 (Temperature)
INFERENCE_TEMPERATURE = 1.0

# ==========================================
# --- 1. 常量与配置 ---
# ==========================================

CAM_CODEC_MAPPING = {
    1: 'H.264', 5: 'H.264', 9: 'H.264',
    2: 'H.265', 6: 'H.265', 10: 'H.265',
    3: 'VP9',   7: 'VP9',   11: 'VP9',
    4: 'AV1',   8: 'AV1',   12: 'AV1'
}

MIN_MIXED_GROUP_SIZE = 4 

DYNAMIC_REWARD_CONFIG = {
    'DYNAMIC_ENABLED': True,
    'FAIRNESS_THRESHOLD': 0.99,
    'NORMAL_EFFICIENCY_WEIGHT': 1.0,
    'NORMAL_FAIRNESS_WEIGHT': 0.5,
    'MIN_QOE_PENALTY_WEIGHT': 5.0,
    'PANIC_EFFICIENCY_WEIGHT': 0.1,
    'PANIC_FAIRNESS_WEIGHT': 2.0
}

ACTION_SPACE = np.round(np.arange(0.5, 2.01, 0.05), 2).tolist()
ACTION_DIM = len(ACTION_SPACE)

CODEC_LIST = ['H.264', 'H.265', 'VP9', 'AV1']
CODEC_DIM = len(CODEC_LIST)

SEQUENCE_LEN = 20
SEQUENCE_FEATURES = 4
# [AMIS-MU 改造] 标量特征增加 2 个: buffer (缓冲), chunks_left (剩余切片)
# 标量特征 = [vmaf, stutter, jitter, avg_weight, buffer, chunks_left] + codec_one_hot
SCALAR_FEATURES = 6 + CODEC_DIM 

PPO_CONFIG = {
    'GAMMA': 0.99,
    'GAE_LAMBDA': 0.95,
    'EPS_CLIP': 0.2,
    'K_EPOCH': 10,
    'LEARNING_RATE': 3e-4,
    'BATCH_SIZE': 64,
    'TRAJ_LEN': 256,
    'C1_VALUE_LOSS': 0.5,
    'C2_ENTROPY_LOSS': 0.05,
    'MODEL_SAVE_DIR': "scratch/RL/paper_repro_model_50/",
    'LSTM_HIDDEN_SIZE': 64
}

CLIENT_TIMEOUT = 30.0

# --- 归一化参数 ---
SEQ_MAX_VALUES = np.array([12000.0, 1000.0, 1.0, 500.0], dtype=np.float32)
SEQ_MIN_VALUES = np.array([0.0, 0.0, 0.0, 0.0], dtype=np.float32)
SEQ_RANGE = SEQ_MAX_VALUES - SEQ_MIN_VALUES
SEQ_RANGE[SEQ_RANGE == 0] = 1.0

# [AMIS-MU 改造] 增加对 Buffer (设最大60秒) 和 Chunks (设最大100个) 的归一化边界
SCALAR_MAX_VALUES = np.array([100.0, 1.0, 100.0, 2.0, 60.0, 100.0] + [1.0] * CODEC_DIM, dtype=np.float32)
SCALAR_MIN_VALUES = np.array([0.0, 0.0, 0.0, 0.5, 0.0, 0.0] + [0.0] * CODEC_DIM, dtype=np.float32)
SCALAR_RANGE = SCALAR_MAX_VALUES - SCALAR_MIN_VALUES
SCALAR_RANGE[SCALAR_RANGE == 0] = 1.0

# ==========================================
# --- 2. 神经网络定义 ---
# ==========================================

class PPOSharedNetwork(nn.Module):
    def __init__(self, seq_features, scalar_features, lstm_hidden_size=64):
        super(PPOSharedNetwork, self).__init__()
        self.conv1 = nn.Conv1d(in_channels=seq_features, out_channels=16, kernel_size=3, padding=1)
        self.conv2 = nn.Conv1d(in_channels=16, out_channels=32, kernel_size=3, padding=1)
        self.pool = nn.AdaptiveAvgPool1d(1)
        self.cnn_out_dim = 32
        
        self.scalar_fc1 = nn.Linear(scalar_features, 32)
        self.scalar_fc2 = nn.Linear(32, 16)
        self.mlp_out_dim = 16
        
        self.combined_dim = self.cnn_out_dim + self.mlp_out_dim
        self.lstm = nn.LSTM(input_size=self.combined_dim, hidden_size=lstm_hidden_size, batch_first=True)
        self.lstm_out_dim = lstm_hidden_size

    def forward(self, x_seq, x_scalar, hidden=None):
        seq_feat = F.relu(self.conv2(F.relu(self.conv1(x_seq))))
        seq_feat = self.pool(seq_feat).squeeze(-1)
        scalar_feat = F.relu(self.scalar_fc2(F.relu(self.scalar_fc1(x_scalar))))
        combined_feat = torch.cat((seq_feat, scalar_feat), dim=1)
        lstm_input = combined_feat.unsqueeze(1)
        
        if hidden is None:
            lstm_out, new_hidden = self.lstm(lstm_input)
        else:
            lstm_out, new_hidden = self.lstm(lstm_input, hidden)
        
        lstm_feat = lstm_out.squeeze(1)
        return lstm_feat, new_hidden

class PPOPolicyNetwork(nn.Module):
    def __init__(self, input_dim, action_dim):
        super(PPOPolicyNetwork, self).__init__()
        self.fc1 = nn.Linear(input_dim, 32)
        self.fc_out = nn.Linear(32, action_dim)
    def forward(self, x):
        return self.fc_out(F.relu(self.fc1(x)))

# [AMIS-MU 改造] 价值网络 (Critic) 额外接收 "分配的权重(即需求C_i)" 作为评估输入
class PPOValueNetwork(nn.Module):
    def __init__(self, input_dim):
        super(PPOValueNetwork, self).__init__()
        # 增加 1 维接收 candidate_weight
        self.fc1 = nn.Linear(input_dim + 1, 32)
        self.fc_out = nn.Linear(32, 1)
        
    def forward(self, x, weight):
        # x: [batch, lstm_out_dim], weight: [batch, 1]
        combined = torch.cat((x, weight), dim=1)
        return self.fc_out(F.relu(self.fc1(combined))).squeeze(-1)

# ==========================================
# --- 3. PPO 智能体类 (核心逻辑) ---
# ==========================================

class PPOAgent:
    def __init__(self, config):
        self.config = config
        self.model_save_dir = config['MODEL_SAVE_DIR']
        self.model_save_path = os.path.join(self.model_save_dir, "paper_repro_lstm_ppo1.pth")
        self.lstm_hidden_size = config['LSTM_HIDDEN_SIZE']
        
        self.shared_network = PPOSharedNetwork(SEQUENCE_FEATURES, SCALAR_FEATURES, self.lstm_hidden_size)
        lstm_out_dim = self.shared_network.lstm_out_dim
        self.policy_network = PPOPolicyNetwork(lstm_out_dim, ACTION_DIM)
        self.value_network = PPOValueNetwork(lstm_out_dim) # 使用改造后的 Critic
        
        self.old_shared_network = PPOSharedNetwork(SEQUENCE_FEATURES, SCALAR_FEATURES, self.lstm_hidden_size)
        self.old_policy_network = PPOPolicyNetwork(lstm_out_dim, ACTION_DIM)
        
        self.load_model()
        self.sync_old_model()
        
        self.optimizer = optim.Adam(
            list(self.shared_network.parameters()) +
            list(self.policy_network.parameters()) + 
            list(self.value_network.parameters()),
            lr=config['LEARNING_RATE']
        )
        self.memory = collections.deque(maxlen=config['TRAJ_LEN'])

    def sync_old_model(self):
        self.old_shared_network.load_state_dict(self.shared_network.state_dict())
        self.old_policy_network.load_state_dict(self.policy_network.state_dict())

    def normalize_state(self, state_dict, group_avg_weight=1.0):
        seq_samples = state_dict.get('metric_samples', [])
        if len(seq_samples) < SEQUENCE_LEN:
            padding = [[0.0] * SEQUENCE_FEATURES] * (SEQUENCE_LEN - len(seq_samples))
            seq_samples = padding + seq_samples
        seq_samples = seq_samples[:SEQUENCE_LEN]
        
        seq_raw = np.array(seq_samples, dtype=np.float32)
        normalized_seq = 2.0 * (seq_raw - SEQ_MIN_VALUES) / SEQ_RANGE - 1.0
        normalized_seq = np.transpose(normalized_seq, (1, 0))
        
        codec_one_hot = self.encode_codec(state_dict.get('codec', 'H.264'))
        
        # [AMIS-MU 改造] 提取 buffer 和 chunks_left
        buffer_val = float(state_dict.get('buffer', 10.0))
        chunks_left_val = float(state_dict.get('left_chunks', 20.0))
        
        scalar_raw = np.array([
            state_dict.get('last_vmaf', 0.0),
            state_dict.get('last_stutter_rate', 0.0),
            state_dict.get('last_vmaf_jitter', 0.0),
            group_avg_weight,
            buffer_val,       # 添加项 1
            chunks_left_val   # 添加项 2
        ] + codec_one_hot, dtype=np.float32)
        
        normalized_scalar = 2.0 * (scalar_raw - SCALAR_MIN_VALUES) / SCALAR_RANGE - 1.0
        
        normalized_seq = np.clip(np.nan_to_num(normalized_seq), -1.0, 1.0)
        normalized_scalar = np.clip(np.nan_to_num(normalized_scalar), -1.0, 1.0)
        
        return torch.FloatTensor(normalized_seq), torch.FloatTensor(normalized_scalar)

    def encode_codec(self, codec_name):
        try:
            index = CODEC_LIST.index(codec_name)
            one_hot = [0.0] * CODEC_DIM
            one_hot[index] = 1.0
            return one_hot
        except ValueError:
            return [0.0] * CODEC_DIM

    def load_model(self):
        if os.path.exists(self.model_save_path):
            try:
                checkpoint = torch.load(self.model_save_path)
                self.shared_network.load_state_dict(checkpoint['shared'])
                self.policy_network.load_state_dict(checkpoint['policy'])
                self.value_network.load_state_dict(checkpoint['value'])
                print(f"✅ 模型已加载: {self.model_save_path}")
            except Exception as e:
                print(f"⚠️ 模型加载失败: {e}")
        else:
            print("📝 初始化新模型.")

    def save_model(self):
        os.makedirs(self.model_save_dir, exist_ok=True)
        torch.save({
            'shared': self.shared_network.state_dict(),
            'policy': self.policy_network.state_dict(),
            'value': self.value_network.state_dict()
        }, self.model_save_path)
        print(f"💾 模型已保存至: {self.model_save_path}")

    def store_transition(self, s_seq, s_scalar, a, r, log_prob, v_pred):
        self.memory.append({
            's_seq': s_seq, 's_scalar': s_scalar, 'a': a, 'r': r, 'log_prob': log_prob, 'v_pred': v_pred
        })

    def compute_gae(self, rewards, values, next_value, gamma, gae_lambda):
        advantages = []
        last_gae = 0.0
        for t in reversed(range(len(rewards))):
            if t == len(rewards) - 1:
                delta = rewards[t] + gamma * next_value - values[t]
            else:
                delta = rewards[t] + gamma * values[t+1] - values[t]
            last_gae = delta + gamma * gae_lambda * last_gae
            advantages.insert(0, last_gae)
        return torch.tensor(advantages, dtype=torch.float32)

    # [AMIS-MU 改造] AMIS-MU Eq.25 边际效用优化器 (贪心求解)
    def optimize_amis_mu(self, squad_ids, squad_cache):
        # AMIS-MU 公式参数
        a1 = 0.05
        a2 = 5.0
        
        utilities = {}
        for cid in squad_ids:
            feat = squad_cache[cid]['feat'] # [1, lstm_dim]
            B = squad_cache[cid]['buffer']
            o = max(1.0, squad_cache[cid]['chunks'])
            
            # 将所有候选权重堆叠，批量通过 Critic 评估 V(s, w)
            weights_tensor = torch.FloatTensor(ACTION_SPACE).unsqueeze(1) # [31, 1]
            feats_expanded = feat.repeat(len(ACTION_SPACE), 1) # [31, lstm_dim]
            
            with torch.no_grad():
                V_vals = self.value_network(feats_expanded, weights_tensor).cpu().numpy()
            
            # 计算边际效用 (Eq. 25)
            denom = (1.0 + a1 * B) * math.log(o + a2)
            utilities[cid] = V_vals / denom

        # 贪心分配算法：初始大家都分配最小权重
        current_alloc_idx = {cid: 0 for cid in squad_ids}
        
        # 限制条件: sum(C_i) = c_total。此处转化为：要求总权重和等于 组内人数 * 1.0 (平均权重1.0)
        target_sum = 1.0 * len(squad_ids)
        current_sum = ACTION_SPACE[0] * len(squad_ids)
        
        # 只要总权重没达到上限，每次给边际效用提升最大的流增加一点权重
        while current_sum < target_sum - 1e-5:
            best_cid = None
            best_marg_util = -float('inf')
            
            for cid in squad_ids:
                curr_idx = current_alloc_idx[cid]
                if curr_idx < len(ACTION_SPACE) - 1: # 还能再加带宽
                    marg = utilities[cid][curr_idx + 1] - utilities[cid][curr_idx]
                    if marg > best_marg_util:
                        best_marg_util = marg
                        best_cid = cid
                        
            if best_cid is not None:
                current_alloc_idx[best_cid] += 1
                current_sum += 0.05
            else:
                break
                
        return current_alloc_idx

    def learn(self, next_state_dict, group_avg_weight, should_save=True):
        if len(self.memory) < self.config['TRAJ_LEN']:
            return
            
        s_seq_batch = torch.stack([m['s_seq'] for m in self.memory])
        s_scalar_batch = torch.stack([m['s_scalar'] for m in self.memory])
        a_batch = torch.LongTensor([m['a'] for m in self.memory])
        
        # [AMIS-MU 改造] 还原行动对应的真实权重，用于 Critic 的评估
        w_batch = torch.FloatTensor([ACTION_SPACE[a] for a in a_batch]).unsqueeze(1)
        
        r_batch = torch.FloatTensor([m['r'] for m in self.memory])
        r_batch = (r_batch - r_batch.mean()) / (r_batch.std() + 1e-8)  # 奖励标准化
        old_log_probs_batch = torch.FloatTensor([m['log_prob'] for m in self.memory])
        v_old_batch = torch.FloatTensor([m['v_pred'] for m in self.memory])
        
        with torch.no_grad():
            s_plus_1_seq, s_plus_1_scalar = self.normalize_state(next_state_dict, group_avg_weight)
            next_feat, _ = self.shared_network(s_plus_1_seq.unsqueeze(0), s_plus_1_scalar.unsqueeze(0))
            # 对于 Next Value，我们暂借 group_avg_weight 作为假定权重来截断
            fake_next_w = torch.FloatTensor([[group_avg_weight]])
            last_value = self.value_network(next_feat, fake_next_w).item()
        
        advantages = self.compute_gae(r_batch.numpy(), v_old_batch.numpy(), last_value, self.config['GAMMA'], self.config['GAE_LAMBDA'])
        returns = advantages + v_old_batch
        advantages = (advantages - advantages.mean()) / (advantages.std() + 1e-8)
        
        for _ in range(self.config['K_EPOCH']):
            feat_new, _ = self.shared_network(s_seq_batch, s_scalar_batch)
            logits_new = self.policy_network(feat_new)
            dist_new = Categorical(logits=logits_new)
            log_probs_new = dist_new.log_prob(a_batch)
            
            log_ratio = log_probs_new - old_log_probs_batch
            # 限制对数差值在 [-20, 10] 之间，防止 exp() 溢出变 inf
            log_ratio = torch.clamp(log_ratio, min=-20.0, max=10.0) 
            ratio = torch.exp(log_ratio)
            surr1 = ratio * advantages
            surr2 = torch.clamp(ratio, 1.0 - self.config['EPS_CLIP'], 1.0 + self.config['EPS_CLIP']) * advantages
            policy_loss = -torch.min(surr1, surr2).mean()
            
            # [AMIS-MU 改造] 传入对应动作的权重，训练 Critic 拟合 V(s, w)
            values_new = self.value_network(feat_new.detach(), w_batch)
            value_loss = F.mse_loss(values_new, returns) * self.config['C1_VALUE_LOSS']
            
            entropy_loss = dist_new.entropy().mean() * self.config['C2_ENTROPY_LOSS']
            total_loss = policy_loss + value_loss - entropy_loss
            
            self.optimizer.zero_grad()
            total_loss.backward()
            nn.utils.clip_grad_norm_(list(self.shared_network.parameters()) + list(self.policy_network.parameters()) + list(self.value_network.parameters()), 0.5)
            self.optimizer.step()
        
        self.sync_old_model()
        self.memory.clear()
        
        mode_str = "TRAIN" if should_save else "INF-LEARN"
        print(f"🌀 [{mode_str}] Updated. Loss: {total_loss.item():.4f}")
        
        if should_save:
            self.save_model()

# ==========================================
# --- 4. 辅助函数 & 组队逻辑 ---
# ==========================================

def jain_index(qoe_list):
    if not qoe_list: return 0.0
    min_qoe = min(qoe_list)
    offset = (abs(min_qoe) + 1.0) if min_qoe < 0 else 0.0
    offset_qoes = [q + offset for q in qoe_list] 
    sum_qoe = sum(offset_qoes)
    sum_qoe_sq = sum([q**2 for q in offset_qoes])
    n = len(offset_qoes)
    if sum_qoe_sq == 0 or n == 0: return 0.0
    jfi = (sum_qoe ** 2) / (n * sum_qoe_sq)
    return jfi

def calculate_group_reward(raw_qoe_list, dynamic_config):
    if not raw_qoe_list: return 0.0, 0.0, 0.0
    fairness_index = jain_index(raw_qoe_list)
    
    eff_weight = dynamic_config['NORMAL_EFFICIENCY_WEIGHT']
    fair_weight = dynamic_config['NORMAL_FAIRNESS_WEIGHT']
    
    if dynamic_config['DYNAMIC_ENABLED'] and fairness_index < dynamic_config['FAIRNESS_THRESHOLD']:
        eff_weight = dynamic_config['PANIC_EFFICIENCY_WEIGHT']
        fair_weight = dynamic_config['PANIC_FAIRNESS_WEIGHT']

    safe_qoe_list = [q + 500.0 for q in raw_qoe_list] 
    sum_qoe_shifted = sum(safe_qoe_list)
    efficiency_reward = eff_weight * math.log(max(sum_qoe_shifted, 0.0) + 1e-6)
    
    fairness_reward = fair_weight * math.log(fairness_index + 1e-6)
    
    min_qoe = min(raw_qoe_list)
    min_qoe_norm = max(0, min_qoe) / 100.0
    penalty_weight = dynamic_config.get('MIN_QOE_PENALTY_WEIGHT', 0.0)
    min_penalty = penalty_weight * (1.0 - min_qoe_norm)
    
    group_reward = efficiency_reward + fairness_reward - min_penalty
    return group_reward, np.var(raw_qoe_list), fairness_index

def find_deterministic_squad(current_cam_id, current_codec, codec_pool):
    my_peers = sorted(list(codec_pool[current_codec]))
    if current_cam_id not in my_peers:
        return None 
    
    my_rank = my_peers.index(current_cam_id)
    squad_ids = [current_cam_id]
    
    for other_codec in CODEC_LIST:
        if other_codec == current_codec:
            continue
        other_peers = sorted(list(codec_pool[other_codec]))
        if my_rank >= len(other_peers):
            return None 
        teammate_id = other_peers[my_rank]
        squad_ids.append(teammate_id)
    
    if len(squad_ids) == MIN_MIXED_GROUP_SIZE:
        return squad_ids
    return None

# ==========================================
# --- 5. 主程序 ---
# ==========================================

def main():
    mode_display = "TRAINING (Save ON)" if TRAIN_MODE else ("INFERENCE-LEARNING (Save OFF)" if INFERENCE_LEARNING else "PURE INFERENCE")
    print(f"✅ 系统启动 | 模式: {mode_display}")
    print(f"🔒 策略: 确定性排序组队 + AMIS-MU Critic分配机制")
    print(f"🌡️  探索温度: {INFERENCE_TEMPERATURE}")

    context = zmq.Context()
    socket = context.socket(zmq.REP)
    socket.bind("tcp://*:5556") 
    
    agent = PPOAgent(PPO_CONFIG)
    
    global_qoe_values = {}  
    global_weights = {}     
    global_pending_exp = {} 
    cam_hidden_states = {}  
    client_last_message_time = {} 
    
    # [AMIS-MU 改造] 全局状态缓存，用于存储整个Squad的LSTM Feature，辅助计算公式25
    squad_state_cache = {}
    
    try:
        DEFAULT_ACTION_INDEX = ACTION_SPACE.index(1.0)
    except:
        DEFAULT_ACTION_INDEX = len(ACTION_SPACE) // 2
    DEFAULT_WEIGHT = ACTION_SPACE[DEFAULT_ACTION_INDEX]

    while True:
        try:
            current_time = time.time()
            active_ids = [cid for cid, t in client_last_message_time.items() if current_time - t <= CLIENT_TIMEOUT]
            
            active_codec_pool = collections.defaultdict(list)
            for cid in active_ids:
                c_type = CAM_CODEC_MAPPING.get(cid, 'H.264')
                active_codec_pool[c_type].append(cid)
            
            to_remove = [k for k in global_weights.keys() if k not in active_ids]
            for k in to_remove:
                del global_weights[k]
                if k in global_qoe_values: del global_qoe_values[k]
                if k in client_last_message_time: del client_last_message_time[k]
                if k in squad_state_cache: del squad_state_cache[k]

            message = socket.recv_json()
            cam_id = message.get('cameraId')
            if cam_id is None:
                socket.send_json({"targetWeight": DEFAULT_WEIGHT})
                continue
                
            client_last_message_time[cam_id] = current_time
            current_codec = CAM_CODEC_MAPPING.get(cam_id, 'H.264')
            
            if cam_id not in active_codec_pool[current_codec]:
                active_codec_pool[current_codec].append(cam_id)
            
            if 'current_state' in message and isinstance(message['current_state'], str):
                try:
                    s_dict = json.loads(message['current_state'])
                    s_dict['codec'] = current_codec
                    message['current_state'] = json.dumps(s_dict)
                except:
                    pass

            if TRAIN_MODE or INFERENCE_LEARNING:
                ind_qoe = message.get('current_reward', 0)
                global_qoe_values[cam_id] = ind_qoe
                
                exp = global_pending_exp.get(cam_id)
                last_state_json = message.get('last_state')
                squad_ids = find_deterministic_squad(cam_id, current_codec, active_codec_pool)
                
                if squad_ids and last_state_json and exp:
                    squad_qoe_list = []
                    valid_reward = True
                    for member_id in squad_ids:
                        if member_id not in global_qoe_values:
                            valid_reward = False; break
                        squad_qoe_list.append(global_qoe_values[member_id])
                    
                    if valid_reward:
                        g_reward, _, _ = calculate_group_reward(squad_qoe_list, DYNAMIC_REWARD_CONFIG)
                        agent.store_transition(
                            exp['s_seq'], exp['s_scalar'], exp['a'], 
                            g_reward, exp['log_prob'], exp['v_pred']
                        )
                        del global_pending_exp[cam_id]
                        
                        if len(agent.memory) >= PPO_CONFIG['TRAJ_LEN']:
                            squad_weights = [global_weights.get(m, DEFAULT_WEIGHT) for m in squad_ids]
                            avg_weight = np.mean(squad_weights)
                            if message.get('current_state'):
                                agent.learn(json.loads(message['current_state']), avg_weight, should_save=TRAIN_MODE)

            # --- D. 推理与 AMIS-MU 分配逻辑 ---
            squad_ids = find_deterministic_squad(cam_id, current_codec, active_codec_pool)
            
            if not squad_ids:
                my_peers_sorted = sorted(active_codec_pool[current_codec])
                rank = my_peers_sorted.index(cam_id) if cam_id in my_peers_sorted else -1
                print(f"⏳ Cam {cam_id} ({current_codec}) Rank {rank} | 等待队友凑齐...")
                socket.send_json({"targetWeight": global_weights.get(cam_id, DEFAULT_WEIGHT)})
                continue

            try:
                state_dict = json.loads(message['current_state'])
                current_squad_weights_for_state = [global_weights.get(m, DEFAULT_WEIGHT) for m in squad_ids]
                avg_weight_input = np.mean(current_squad_weights_for_state)

                # 1. 获取网络基础特征
                s_seq, s_scalar = agent.normalize_state(state_dict, avg_weight_input)
                
                
                # 2. 生成 LSTM 特征缓存 (供 AMIS-MU 公式使用)
                with torch.no_grad():
                    feat, _ = agent.shared_network(s_seq.unsqueeze(0), s_scalar.unsqueeze(0))
                
                
                squad_state_cache[cam_id] = {
                    'feat': feat,
                    'buffer': float(state_dict.get('buffer', 10.0)),
                    'chunks': float(state_dict.get('left_chunks', 20.0))
                }

                # 3. 判断是否满足全局分配条件
                final_action_idx = None
                is_forced_by_amis = False
                
                # [核心结合点] 缓存就绪，且以 20% 的概率触发 AMIS-MU 引导，剩下 80% 留给 PPO 自己探索
                if all(m in squad_state_cache for m in squad_ids) and random.random() < 0.3:

                    optimal_allocs = agent.optimize_amis_mu(squad_ids, squad_state_cache)
                    if optimal_allocs is not None:
                        final_action_idx = optimal_allocs[cam_id]
                        is_forced_by_amis = True  # 标记该动作不是网络自己选的
                        print(f"🌟 [AMIS-MU] Eq. 25 求解完成. Cam {cam_id} 获取分配 idx: {final_action_idx}")

                # 4. 获取 Actor 的基础探索输出 (用于计算 log_prob 及填补后备逻辑)
                with torch.no_grad():
                    logits = torch.clamp(agent.policy_network(feat), min=-10.0, max=10.0)
                    dist = Categorical(logits=logits / INFERENCE_TEMPERATURE)
                    
                # 如果前置的 AMIS-MU 贪心分配没有执行，回退到 Actor 的自身输出
                if final_action_idx is None:
                    final_action_idx = dist.sample().item() if (TRAIN_MODE or INFERENCE_LEARNING) else logits.argmax().item()
                
                # 5. 生成 RL 训练所必需的指标 (根据选择的 final_action 提取 Critic 评分)
                with torch.no_grad():
                    raw_log_prob = dist.log_prob(torch.tensor(final_action_idx)).item()
                    log_prob = max(raw_log_prob, -20.0)  # 防止被迫动作产生极度负的 log_prob
                    chosen_weight = ACTION_SPACE[final_action_idx]
                    w_tensor = torch.FloatTensor([[chosen_weight]])
                    v_pred = agent.value_network(feat, w_tensor).item()
                
                is_learning_active = TRAIN_MODE or INFERENCE_LEARNING

                if is_learning_active and not is_forced_by_amis:
                    global_pending_exp[cam_id] = {
                        's_seq': s_seq, 's_scalar': s_scalar,
                        'a': final_action_idx, 'log_prob': log_prob, 'v_pred': v_pred
                    }
                
                # 我们依然利用 SquadMean 约束它，防止整体带宽崩溃
                temp_weights = {m: global_weights.get(m, DEFAULT_WEIGHT) for m in squad_ids}
                temp_weights[cam_id] = chosen_weight 
                squad_mean_weight = np.mean(list(temp_weights.values())) + 1e-6
                scale_factor = 1.0 / squad_mean_weight
                final_weight = np.clip(chosen_weight * scale_factor, ACTION_SPACE[0], ACTION_SPACE[-1])
                
                global_weights[cam_id] = final_weight 
                
                print(f"🚀 Cam {cam_id} ({current_codec}) | Squad: {squad_ids}")
                print(f"   Action: {chosen_weight:.2f} | SquadMean: {squad_mean_weight:.2f} -> Final: {final_weight:.4f}")
                
                socket.send_json({"targetWeight": final_weight})

            except Exception as e:
                print(f"Error inference CAM {cam_id}: {e}")
                import traceback
                traceback.print_exc()
                socket.send_json({"targetWeight": DEFAULT_WEIGHT})

        except zmq.error.ZMQError:
            continue
        except KeyboardInterrupt:
            print("退出...")
            if TRAIN_MODE: agent.save_model()
            sys.exit(0)
        except Exception as e:
            import traceback
            traceback.print_exc()
            print(f"Main loop error: {e}")

if __name__ == '__main__':
    main()