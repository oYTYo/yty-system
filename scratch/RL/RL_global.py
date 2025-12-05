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

# --- 1. 常量与配置 ---

# (新) 分组配置: 将 Camera ID 映射到 Group ID
# 团队1: {1(H264), 2(H265), 3(VP9), 4(AV1)}
# 团队2: {5(H264), 6(H265), 7(VP9), 8(AV1)}
# 团队3: {9(H264), 10(H265), 11(VP9), 12(AV1)}
GROUP_CONFIG = {
    1: 1, 2: 1, 3: 1, 4: 1,
    5: 2, 6: 2, 7: 2, 8: 2,
    9: 3, 10: 3, 11: 3, 12: 3,
}
NUM_GROUPS = 3

# --- (新) 动态奖励权重配置 ---
# 描述: 这是一个动态权重系统。
# 当Jain's Index (JFI) 低于 DYNAMIC_FAIRNESS_THRESHOLD 时，
# 系统会 "恐慌" 并将 FAIRNESS_PENALTY_WEIGHT (例如 5.0) 
# 应用于公平性奖励，同时压低效率奖励，迫使智能体优先解决公平问题。
# 当 JFI 恢复到阈值以上时，权重恢复正常。
DYNAMIC_REWARD_CONFIG = {
    'DYNAMIC_ENABLED': True,           # 启用动态权重
    'FAIRNESS_THRESHOLD': 0.85,        # (可调) JFI 低于此值时, 触发 "恐慌"
    
    # 正常模式 (JFI >= 0.85)
    'NORMAL_EFFICIENCY_WEIGHT': 1.0,   # (可调) 论文 [TOMM.pdf] 值为 1.0
    'NORMAL_FAIRNESS_WEIGHT': 0.5,     # (可调) 论文 [TOMM.pdf] 值为 0.5
    
    # 恐慌模式 (JFI < 0.85)
    'PANIC_EFFICIENCY_WEIGHT': 0.1,    # (可调) 显著降低效率的重要性
    'PANIC_FAIRNESS_WEIGHT': 2.0       # (可调) 显著提高公平性的重要性
}

# (旧) 智能体动作/状态配置
ACTION_SPACE = np.round(np.arange(0.5, 2.01, 0.05), 2).tolist()
ACTION_DIM = len(ACTION_SPACE)
CODEC_LIST = ['H.264', 'H.265', 'VP9', 'AV1']
CODEC_DIM = len(CODEC_LIST)
SEQUENCE_LEN = 20
SEQUENCE_FEATURES = 4
# 状态包含: 3个QoE指标 + 1个组平均权重 + Codec
SCALAR_FEATURES = 4 + CODEC_DIM 

# (旧) PPO 训练配置
PPO_CONFIG = {
    'GAMMA': 0.99, 'GAE_LAMBDA': 0.95, 'EPS_CLIP': 0.2, 'K_EPOCH': 10,
    'LEARNING_RATE': 3e-4, 'BATCH_SIZE': 64, 'TRAJ_LEN': 256,
    'C1_VALUE_LOSS': 0.5, 'C2_ENTROPY_LOSS': 0.01, 
    'MODEL_SAVE_DIR': "scratch/RL/shared_policy/" # (新) 单一模型保存路径
}
CLIENT_TIMEOUT = 30.0

# --- 1.5. 状态归一化配置 (与上一版相同) ---
SEQ_MAX_VALUES = np.array([12000.0, 1000.0, 1.0, 500.0], dtype=np.float32)
SEQ_MIN_VALUES = np.array([0.0, 0.0, 0.0, 0.0], dtype=np.float32)
SEQ_RANGE = SEQ_MAX_VALUES - SEQ_MIN_VALUES
SEQ_RANGE[SEQ_RANGE == 0] = 1.0

# 标量范围 (VMAF, Stutter, VMAF_Jitter, Group_Avg_Weight, Codec_One_Hot)
SCALAR_MAX_VALUES = np.array([100.0, 1.0, 100.0, 2.0] + [1.0] * CODEC_DIM, dtype=np.float32)
SCALAR_MIN_VALUES = np.array([0.0, 0.0, 0.0, 0.5] + [0.0] * CODEC_DIM, dtype=np.float32)
SCALAR_RANGE = SCALAR_MAX_VALUES - SCALAR_MIN_VALUES
SCALAR_RANGE[SCALAR_RANGE == 0] = 1.0

# --- 2. 神经网络 (不变) ---
class PPOSharedNetwork(nn.Module):
    def __init__(self, seq_features, scalar_features):
        super(PPOSharedNetwork, self).__init__()
        self.conv1 = nn.Conv1d(in_channels=seq_features, out_channels=16, kernel_size=3, padding=1)
        self.conv2 = nn.Conv1d(in_channels=16, out_channels=32, kernel_size=3, padding=1)
        self.pool = nn.AdaptiveAvgPool1d(1)
        self.cnn_out_dim = 32
        self.scalar_fc1 = nn.Linear(scalar_features, 32)
        self.scalar_fc2 = nn.Linear(32, 16)
        self.mlp_out_dim = 16
        self.combined_dim = self.cnn_out_dim + self.mlp_out_dim
    def forward(self, x_seq, x_scalar):
        seq_feat = F.relu(self.conv2(F.relu(self.conv1(x_seq))))
        seq_feat = self.pool(seq_feat).squeeze(-1)
        scalar_feat = F.relu(self.scalar_fc2(F.relu(self.scalar_fc1(x_scalar))))
        combined_feat = torch.cat((seq_feat, scalar_feat), dim=1)
        return combined_feat

class PPOPolicyNetwork(nn.Module):
    def __init__(self, combined_dim, action_dim):
        super(PPOPolicyNetwork, self).__init__()
        self.fc1 = nn.Linear(combined_dim, 32)
        self.fc_out = nn.Linear(32, action_dim)
    def forward(self, combined_features):
        return self.fc_out(F.relu(self.fc1(combined_features)))

class PPOValueNetwork(nn.Module):
    def __init__(self, combined_dim):
        super(PPOValueNetwork, self).__init__()
        self.fc1 = nn.Linear(combined_dim, 32)
        self.fc_out = nn.Linear(32, 1)
    def forward(self, combined_features):
        return self.fc_out(F.relu(self.fc1(combined_features))).squeeze(-1)

# --- 3. PPO 智能体类 (修改) ---
class PPOAgent:
    # (新) 构造函数不再需要 group_id
    def __init__(self, config):
        self.config = config
        
        # (新) 单一策略的模型路径
        self.model_save_dir = config['MODEL_SAVE_DIR']
        self.model_save_path = os.path.join(self.model_save_dir, f"shared_policy_ppo_cnn.pth")
        
        self.shared_network = PPOSharedNetwork(SEQUENCE_FEATURES, SCALAR_FEATURES)
        combined_dim = self.shared_network.combined_dim
        self.policy_network = PPOPolicyNetwork(combined_dim, ACTION_DIM)
        self.value_network = PPOValueNetwork(combined_dim)
        self.old_shared_network = PPOSharedNetwork(SEQUENCE_FEATURES, SCALAR_FEATURES)
        self.old_policy_network = PPOPolicyNetwork(combined_dim, ACTION_DIM)
        
        self.load_model() # 加载单一模型
        
        self.old_shared_network.load_state_dict(self.shared_network.state_dict())
        self.old_policy_network.load_state_dict(self.policy_network.state_dict())
        self.optimizer = optim.Adam(
            list(self.shared_network.parameters()) +
            list(self.policy_network.parameters()) + 
            list(self.value_network.parameters()),
            lr=config['LEARNING_RATE']
        )
        self.memory = collections.deque(maxlen=config['TRAJ_LEN'])
        self.shared_network.train()
        self.policy_network.train()
        self.value_network.train()

    # (不变) 归一化函数, 接收 组平均权重 作为上下文
    def normalize_state(self, state_dict, group_avg_weight=1.0):
        seq_samples = state_dict.get('metric_samples', [])
        if len(seq_samples) < SEQUENCE_LEN:
            padding = [[0.0] * SEQUENCE_FEATURES] * (SEQUENCE_LEN - len(seq_samples))
            seq_samples = padding + seq_samples
        seq_samples = seq_samples[:SEQUENCE_LEN]
        seq_raw = np.array(seq_samples, dtype=np.float32)
        normalized_seq = 2.0 * (seq_raw - SEQ_MIN_VALUES) / SEQ_RANGE - 1.0
        normalized_seq = np.transpose(normalized_seq, (1, 0))
        
        codec_one_hot = encode_codec(state_dict.get('codec', 'H.264'))
        
        scalar_raw = np.array([
            state_dict.get('last_vmaf', 0.0),
            state_dict.get('last_stutter_rate', 0.0),
            state_dict.get('last_vmaf_jitter', 0.0),
            group_avg_weight  # 组上下文特征
        ] + codec_one_hot, dtype=np.float32)
        
        normalized_scalar = 2.0 * (scalar_raw - SCALAR_MIN_VALUES) / SCALAR_RANGE - 1.0
        normalized_seq = np.clip(normalized_seq, -1.0, 1.0)
        normalized_scalar = np.clip(normalized_scalar, -1.0, 1.0)
        return torch.FloatTensor(normalized_seq), torch.FloatTensor(normalized_scalar)

    # (不变)
    def select_action(self, state_seq, state_scalar, is_training=True):
        state_seq_tensor = state_seq.unsqueeze(0)
        state_scalar_tensor = state_scalar.unsqueeze(0)
        with torch.no_grad():
            combined_feat = self.shared_network(state_seq_tensor, state_scalar_tensor)
            logits = self.policy_network(combined_feat)
        if not is_training:
            return logits.argmax().item(), None, None
        else:
            with torch.no_grad():
                value = self.value_network(combined_feat).item()
            dist = Categorical(logits=logits)
            action_index = dist.sample().item()
            log_prob = dist.log_prob(torch.tensor(action_index)).item()
            return action_index, log_prob, value

    # (不变)
    def store_transition(self, s_seq, s_scalar, a, r, log_prob, v_pred):
        self.memory.append({
            's_seq': s_seq, 's_scalar': s_scalar, 
            'a': a, 'r': r, 'log_prob': log_prob, 'v_pred': v_pred
        })

    # (修改) 加载单一模型
    def load_model(self):
        path = self.model_save_path
        if os.path.exists(path):
            try:
                if os.path.getsize(path) == 0:
                    print(f"⚠️ [GlobalAgent] 警告: 模型文件 {path} 存在, 但大小为 0。")
                    return False
                checkpoint = torch.load(path)
                self.shared_network.load_state_dict(checkpoint['shared_state_dict'])
                self.policy_network.load_state_dict(checkpoint['policy_state_dict'])
                self.value_network.load_state_dict(checkpoint['value_state_dict'])
                print(f"✅ [GlobalAgent] 成功从 {path} 加载共享策略模型参数。")
                return True
            except Exception as e:
                print(f"⚠️ [GlobalAgent] 警告: 加载模型失败 ({e})。文件可能损坏或与网络结构不匹配。")
                return False
        else:
            print(f"📝 [GlobalAgent] 未找到历史模型文件 {path}。将使用随机初始化开始新训练。")
            return False

    # (修改) 保存单一模型
    def save_model(self):
        os.makedirs(self.model_save_dir, exist_ok=True)
        checkpoint = {
            'shared_state_dict': self.shared_network.state_dict(),
            'policy_state_dict': self.policy_network.state_dict(),
            'value_state_dict': self.value_network.state_dict(),
        }
        torch.save(checkpoint, self.model_save_path)
        print(f"✅ [GlobalAgent] 共享策略 PPO 模型参数已成功保存到 {self.model_save_path}。")

    # (不变)
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

    # (修改) learn 函数接收 组平均权重
    def learn(self, next_state_dict, group_avg_weight):
        if len(self.memory) < self.config['TRAJ_LEN']:
            return
        
        s_seq_batch = torch.stack([m['s_seq'] for m in self.memory])
        s_scalar_batch = torch.stack([m['s_scalar'] for m in self.memory])
        a_batch = torch.LongTensor([m['a'] for m in self.memory])
        r_batch = torch.FloatTensor([m['r'] for m in self.memory])
        old_log_probs_batch = torch.FloatTensor([m['log_prob'] for m in self.memory])
        v_old_batch = torch.FloatTensor([m['v_pred'] for m in self.memory])
        
        with torch.no_grad():
            # (新) 使用 group_avg_weight 归一化 next_state
            s_plus_1_seq, s_plus_1_scalar = self.normalize_state(next_state_dict, group_avg_weight)
            next_combined_feat = self.shared_network(s_plus_1_seq.unsqueeze(0), s_plus_1_scalar.unsqueeze(0))
            last_value = self.value_network(next_combined_feat).item()
            
        advantages = self.compute_gae(r_batch.numpy(), v_old_batch.numpy(), last_value, self.config['GAMMA'], self.config['GAE_LAMBDA'])
        returns = advantages + v_old_batch
        advantages = (advantages - advantages.mean()) / (advantages.std() + 1e-8)
        
        for _ in range(self.config['K_EPOCH']):
            combined_feat_new = self.shared_network(s_seq_batch, s_scalar_batch)
            logits_new = self.policy_network(combined_feat_new)
            dist_new = Categorical(logits=logits_new)
            log_probs_new = dist_new.log_prob(a_batch)
            ratio = torch.exp(log_probs_new - old_log_probs_batch)
            surr1 = ratio * advantages
            surr2 = torch.clamp(ratio, 1.0 - self.config['EPS_CLIP'], 1.0 + self.config['EPS_CLIP']) * advantages
            policy_loss = -torch.min(surr1, surr2).mean()
            values_new = self.value_network(combined_feat_new)
            value_loss = F.mse_loss(values_new, returns) * self.config['C1_VALUE_LOSS']
            entropy_loss = dist_new.entropy().mean() * self.config['C2_ENTROPY_LOSS']
            total_loss = policy_loss + value_loss - entropy_loss
            self.optimizer.zero_grad()
            total_loss.backward()
            nn.utils.clip_grad_norm_(self.shared_network.parameters(), 0.5)
            nn.utils.clip_grad_norm_(self.policy_network.parameters(), 0.5)
            nn.utils.clip_grad_norm_(self.value_network.parameters(), 0.5)
            self.optimizer.step()
            
        self.old_shared_network.load_state_dict(self.shared_network.state_dict())
        self.old_policy_network.load_state_dict(self.policy_network.state_dict())
        self.memory.clear()
        print(f"🌀 [GlobalAgent] 共享策略学习完成, P_Loss: {policy_loss.item():.4f}, V_Loss: {value_loss.item():.4f}")
        self.save_model()

# --- 4. 辅助函数 (新) ---

def jain_index(qoe_list):
    """ 计算 Jain's Fairness Index """
    if not qoe_list or len(qoe_list) == 0:
        return 0.0
    # 为避免 QoE 为 0 或负数时 JFI 计算出错, 增加一个偏移量
    offset_qoes = [q + 500.0 for q in qoe_list] # 假设 QoE 不会低于 -500
    sum_qoe = sum(offset_qoes)
    sum_qoe_sq = sum([q**2 for q in offset_qoes])
    n = len(offset_qoes)
    
    if sum_qoe_sq == 0 or n == 0:
        return 0.0 # 避免除以 0
        
    jfi = (sum_qoe ** 2) / (n * sum_qoe_sq)
    return jfi

def calculate_group_reward(raw_qoe_list, dynamic_config):
    """
    (新) 计算 组内 奖励 (动态权重 效率 + 公平)
    """
    if not raw_qoe_list: 
        return 0.0, 0.0, 0.0 # 返回 (奖励, 方差, JFI)

    # 0. 计算公平性指标
    fairness_index = jain_index(raw_qoe_list)
    
    # 1. (新) 根据JFI动态选择权重
    eff_weight = dynamic_config['NORMAL_EFFICIENCY_WEIGHT']
    fair_weight = dynamic_config['NORMAL_FAIRNESS_WEIGHT']
    
    if dynamic_config['DYNAMIC_ENABLED'] and fairness_index < dynamic_config['FAIRNESS_THRESHOLD']:
        # 进入 "恐慌" 模式, 优先保证公平
        eff_weight = dynamic_config['PANIC_EFFICIENCY_WEIGHT']
        fair_weight = dynamic_config['PANIC_FAIRNESS_WEIGHT']

    # 2. 效率项 (Efficiency)
    safe_qoe_list = [q + 500.0 for q in raw_qoe_list]
    sum_qoe_shifted = sum(safe_qoe_list)
    efficiency_reward = eff_weight * math.log(sum_qoe_shifted + 1e-6) # log(sum)
    
    # 3. 公平项 (Fairness)
    # 使用 log(JFI) 作为奖励, JFI 接近0时惩罚巨大, 接近1时奖励接近0
    fairness_reward = fair_weight * math.log(fairness_index + 1e-6) # log(JFI)
    
    # 4. 总奖励
    group_reward = efficiency_reward + fairness_reward
    
    # 5. 仅用于日志的方差
    group_variance = np.var(raw_qoe_list)
    
    return group_reward, group_variance, fairness_index

def log_progress(log_file_path, group_reward, group_variance, jfi):
    """ (新) 记录日志, 写入特定组的文件 """
    try:
        if not os.path.exists(os.path.dirname(log_file_path)):
            os.makedirs(os.path.dirname(log_file_path))
        with open(log_file_path, 'a') as f:
            timestamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
            f.write(f"{timestamp},{group_reward:.4f},{group_variance:.4f},{jfi:.4f}\n")
    except Exception as e:
        print(f"警告: 无法写入日志文件 {log_file_path}: {e}")

def encode_codec(codec_name):
    """ (不变) Codec 独热编码 """
    try:
        index = CODEC_LIST.index(codec_name)
        one_hot = [0.0] * CODEC_DIM
        one_hot[index] = 1.0
        return one_hot
    except ValueError:
        return [0.0] * CODEC_DIM

# --- 5. ZMQ 服务器主逻辑 (新) ---
def main():
    print(f"AI Agent (参数共享 分组MARL架构) 已启动。")
    print(f"分组配置: {GROUP_CONFIG}")
    print(f"状态维度 (Seq: {SEQUENCE_LEN}x{SEQUENCE_FEATURES}, Scalar: {SCALAR_FEATURES})。")
    print(f"动态奖励配置: {DYNAMIC_REWARD_CONFIG}")

    context = zmq.Context()
    socket = context.socket(zmq.REP)
    socket.bind("tcp://*:5556")
    print(f"RL服务器(共享策略模型)已启动，正在 tcp://localhost:5556 等待连接...")

    # --- (新) 初始化 *一个* 共享智能体 ---
    print(f"--- 正在初始化 Global Shared Agent ---")
    global_agent = PPOAgent(PPO_CONFIG)
    
    # --- (新) 初始化所有组的 *数据跟踪器* ---
    group_pending_experience = {} # 存储 {group_id -> {cam_id -> experience}}
    group_qoe_values = {}       # 存储 {group_id -> {cam_id -> qoe}}
    group_weights = {}          # 存储 {group_id -> {cam_id -> weight}}
    group_log_files = {}        # 存储 {group_id -> "path/to/log.csv"}

    for i in range(1, NUM_GROUPS + 1):
        group_id = i
        group_pending_experience[group_id] = {}
        group_qoe_values[group_id] = {}
        group_weights[group_id] = {}
        
        # 日志文件仍然按组分开, 方便调试
        log_file_path = os.path.join(PPO_CONFIG['MODEL_SAVE_DIR'], f"group_{group_id}_progress_log.csv")
        group_log_files[group_id] = log_file_path
        
        if not os.path.exists(log_file_path):
            try:
                os.makedirs(os.path.dirname(log_file_path), exist_ok=True)
                with open(log_file_path, 'w') as f:
                    f.write("Timestamp,Group_Reward,QoE_Variance,Jain_Index\n")
                print(f"日志文件已初始化: {log_file_path}")
            except Exception as e:
                print(f"警告: 无法初始化日志文件 {log_file_path}: {e}")

    client_last_message_time = {}

    #############################################################################################################################
    ONLINE_TRAINING_ENABLED = True
    
    try:
        DEFAULT_ACTION_INDEX = ACTION_SPACE.index(1.0)
    except ValueError:
        DEFAULT_ACTION_INDEX = len(ACTION_SPACE) // 2
    DEFAULT_WEIGHT = ACTION_SPACE[DEFAULT_ACTION_INDEX]

    # --- (新) 主循环: 共享策略, 分组处理 ---
    while True:
        try:
            current_time = time.time()
            dead_clients = [
                cam_id for cam_id, last_time in client_last_message_time.items()
                if current_time - last_time > CLIENT_TIMEOUT
            ]
            if dead_clients:
                print(f"--- 清理 {len(dead_clients)} 个超时客户端: {dead_clients} ---")
                for cam_id in dead_clients:
                    client_last_message_time.pop(cam_id, None)
                    group_id = GROUP_CONFIG.get(cam_id)
                    if group_id:
                        group_pending_experience[group_id].pop(cam_id, None)
                        group_qoe_values[group_id].pop(cam_id, None)
                        group_weights[group_id].pop(cam_id, None)

            message = socket.recv_json()
        except zmq.error.ZMQError as e:
            print(f"ZMQ 接收错误: {e}")
            continue

        cam_id = message.get('cameraId')
        if cam_id is None:
            socket.send_json({"targetWeight": DEFAULT_WEIGHT}) 
            continue
            
        client_last_message_time[cam_id] = current_time

        # --- 1. 确定客户端所属的组 ---
        group_id = GROUP_CONFIG.get(cam_id)
        if group_id is None:
            print(f"警告: 摄像头 {cam_id} 没有配置分组, 已忽略。")
            socket.send_json({"targetWeight": DEFAULT_WEIGHT})
            continue

        # --- 2. 获取该组专用的 *数据跟踪器* ---
        agent = global_agent # (新) 智能体是全局共享的
        qoe_dict = group_qoe_values[group_id]
        weight_dict = group_weights[group_id]
        exp_dict = group_pending_experience[group_id]
        log_file = group_log_files[group_id]

        if cam_id not in qoe_dict:
            qoe_dict[cam_id] = 0
            weight_dict[cam_id] = DEFAULT_WEIGHT
            exp_dict[cam_id] = None

        # --- 3. 计算 组内 上下文 (组平均权重) ---
        active_weights = list(weight_dict.values())
        group_avg_weight = np.mean(active_weights) if active_weights else DEFAULT_WEIGHT

        # --- 4. 奖励计算 / 经验存储 (基于组) ---
        individual_qoe = message.get('current_reward')
        if individual_qoe is None:
            individual_qoe = 0 
        qoe_dict[cam_id] = individual_qoe # 更新 组内QoE 字典

        last_state_json = message.get('last_state')
        exp = exp_dict.get(cam_id) # 获取该客户端 上周期的经验
        
        if last_state_json and last_state_json != "{}" and exp is not None and ONLINE_TRAINING_ENABLED:
            group_qoe_list = list(qoe_dict.values())
            # (新) 使用动态权重奖励函数
            group_reward, group_variance, jfi = calculate_group_reward(group_qoe_list, DYNAMIC_REWARD_CONFIG)
            
            log_progress(log_file, group_reward, group_variance, jfi)
            
            print(f"CAM {cam_id} [G{group_id}]: 存入经验. 组奖励={group_reward:.3f} (组方差:{group_variance:.2f}, JFI:{jfi:.3f}). (个体QoE:{individual_qoe:.2f})")

            # (新) 存储 (s_last, a_last, r_group) 到 *全局经验池*
            agent.store_transition(
                exp['s_seq'], exp['s_scalar'], 
                exp['a'], group_reward, exp['log_prob'], exp['v_pred']
            )

            # (新) 触发 *全局智能体* 的学习
            if len(agent.memory) >= PPO_CONFIG['TRAJ_LEN']:
                print(f"🌀 [GlobalAgent] 经验池已满, CAM {cam_id} [G{group_id}] 触发 全局学习...")
                current_state_json_str = message.get('current_state')
                if current_state_json_str:
                    try:
                        next_state_dict = json.loads(current_state_json_str)
                        # (新) learn 函数需要 next_state_dict 和 *当前组* 的 group_avg_weight
                        # (注意: next_state_dict 也是 cam_id 的, 所以用 group_avg_weight 是正确的)
                        agent.learn(next_state_dict, group_avg_weight)
                    except Exception as e:
                        print(f"错误: [GlobalAgent] 学习时解析 'current_state' JSON 失败: {e}")
                else:
                    print(f"警告: [GlobalAgent] 触发学习, 但 'current_state' 为空。")

        # --- 5. 状态构建 / 动作选择 (使用全局策略) ---
        current_state_json_str = message.get('current_state')
        if not current_state_json_str:
            print(f"警告: CAM {cam_id} [G{group_id}] 消息缺少 'current_state'。")
            socket.send_json({"targetWeight": DEFAULT_WEIGHT})
            continue

        try:
            state_dict = json.loads(current_state_json_str)
        except Exception as e:
            print(f"错误: CAM {cam_id} [G{group_id}] 无法解析 'current_state' JSON: {e}")
            socket.send_json({"targetWeight": DEFAULT_WEIGHT})
            continue

        try:
            s_t_seq, s_t_scalar = agent.normalize_state(state_dict, group_avg_weight)
        except Exception as e:
            print(f"错误: [G{group_id}] 状态归一化失败: {e}, state_dict: {state_dict}")
            socket.send_json({"targetWeight": DEFAULT_WEIGHT})
            continue

        if ONLINE_TRAINING_ENABLED:
            # (新) 使用 *全局智能体* 决策
            action_index, log_prob, v_pred = agent.select_action(s_t_seq, s_t_scalar, is_training=True)
            exp_dict[cam_id] = {
                's_seq': s_t_seq, 's_scalar': s_t_scalar,
                'a': action_index, 'log_prob': log_prob, 'v_pred': v_pred
            }
        else:
            action_index, _, _ = agent.select_action(s_t_seq, s_t_scalar, is_training=False)
            exp_dict[cam_id] = None

        # --- (!!!) 核心修改: 组权重标准化 (!!!) ---
        
        # 1. 获取智能体选择的 *原始* 权重
        chosen_weight_raw = ACTION_SPACE[action_index]
        
        # 2. 将原始权重 *临时* 放入组字典, 以便计算
        weight_dict[cam_id] = chosen_weight_raw
        
        # 3. 获取当前组的 N 维权重向量 (N=组内摄像头数)
        current_group_weights = list(weight_dict.values())
        
        final_chosen_weight = chosen_weight_raw # 最终发送的权重
        
        if not current_group_weights:
            # 组是空的 (不太可能发生, 但做个保护)
            socket.send_json({"targetWeight": final_chosen_weight})
        else:
            # 4. 计算当前组的 *原始* 均值
            current_mean = np.mean(current_group_weights)
            
            # 5. 计算缩放因子, 目标是使均值 = 1 (避免除以 0)
            scale_factor = 1.0 / (current_mean + 1e-6)
            
            # 6. 标准化 (缩放) *整个组* 的所有权重
            # (注意: 我们必须遍历字典的键来更新, 而不是列表)
            for w_cam_id in weight_dict.keys():
                # 计算标准化后的权重
                normalized_weight = weight_dict[w_cam_id] * scale_factor
                
                # 裁剪: 确保标准化后的权重不会超出原始动作空间 (0.5 ~ 2.0)
                # 这是一个安全措施, 防止极端值
                normalized_weight = np.clip(normalized_weight, ACTION_SPACE[0], ACTION_SPACE[-1])
                
                # 更新字典中的值为标准化后的值
                weight_dict[w_cam_id] = normalized_weight 
                
                # 记录 *当前摄像头* 应该被发送回去的最终值
                if w_cam_id == cam_id:
                    final_chosen_weight = normalized_weight
            
            # 7. 发送这个被标准化和裁剪后的权重
            socket.send_json({"targetWeight": final_chosen_weight})
            
            # (新) 更新 组平均权重 (它现在应该非常接近 1, 除非被裁剪)
            group_avg_weight = np.mean(list(weight_dict.values()))
        
        # --- (!!!) 修改结束 (!!!) ---
        
        print(f"CAM {cam_id} [G{group_id}]: 回复. 推荐权重 {final_chosen_weight:.2f} (组均重: {group_avg_weight:.2f})\n")

if __name__ == '__main__':
    print(f"当前 PyTorch 版本: {torch.__version__}")
    main()