import pandas as pd
import os

# --- 配置 ---
input_txt_path = 'scratch/play_status_large_scale.txt'
output_excel_path = 'scratch/play_status_large_scale_processed.xlsx'
# --- 配置结束 ---

def convert_txt_to_excel(txt_file, excel_file):
    if not os.path.exists(txt_file):
        print(f"错误：文件 {txt_file} 不存在")
        return

    print(f"读取文件: {txt_file}")
    
    # 1. [诊断] 打印文件前3行的“真实内容”（包含隐藏字符）
    #    这能帮我们确认是否真的是 \t 分隔，还是混入了其他东西
    print("\n--- [诊断] 文件前3行原始内容 (repr) ---")
    with open(txt_file, 'r', encoding='utf-8', errors='ignore') as f:
        for i in range(3):
            line = f.readline()
            if not line: break
            print(repr(line)) # repr() 会把 \t, \n 等显示出来
    print("----------------------------------------\n")

    try:
        # 2. 读取数据
        #    既然你确定是 \t，我们就显式指定 sep='\t'。
        #    engine='python' 更稳定，且能避免一些 C 引擎的警告。
        df = pd.read_csv(txt_file, sep='\t', engine='python')
        
        print(f"读取到 {len(df)} 行原始数据")
        
        # 3. [诊断] 检查列名
        #    有时候列名里会有隐藏的空格，比如 'Time(s) '，导致索引失败
        df.columns = df.columns.str.strip() # 去除列名首尾空格
        print(f"检测到的列名: {df.columns.tolist()}")

        if 'Time(s)' not in df.columns:
            print("错误：找不到 'Time(s)' 列！可能是列名解析错误。")
            return

        # 4. [诊断] 打印转换前的 Time(s) 数据样本
        print("\n--- [诊断] Time(s) 列前5个原始值 ---")
        print(df['Time(s)'].head())

        # 5. 强力清洗与转换
        #    (1) 转为字符串 (2) 去除首尾空格/换行符 (3) 替换逗号 (4) 转数字
        df['Time(s)'] = df['Time(s)'].astype(str).str.strip().str.replace(',', '.', regex=False)
        df['Time(s)'] = pd.to_numeric(df['Time(s)'], errors='coerce')

        # 6. 检查转换结果
        nan_count = df['Time(s)'].isna().sum()
        valid_count = len(df) - nan_count
        print(f"\n成功转换的时间数据: {valid_count} 条")
        print(f"无法识别的坏数据: {nan_count} 条")

        if valid_count == 0:
            print("!!! 严重错误：所有时间数据都无法识别。请检查上面的[诊断]信息。 !!!")
            return

        # 7. 删除坏数据行
        df = df.dropna(subset=['Time(s)'])

        # 8. 过滤前60秒
        print("\n正在过滤前60秒数据...")
        df_final = df[df['Time(s)'] >= 60].copy()
        
        print(f"过滤前条数: {len(df)}")
        print(f"过滤后条数 (>=60s): {len(df_final)}")

        if len(df_final) == 0:
            print(f"警告：结果为空。可能数据最大时间 ({df['Time(s)'].max()}) 小于 60 秒。")
        else:
            print(f"\n正在写入Excel: {excel_file} ...")
            df_final.to_excel(excel_file, index=False)
            print("转换成功！")

    except Exception as e:
        print(f"\n发生异常: {e}")
        import traceback
        traceback.print_exc()

if __name__ == "__main__":
    convert_txt_to_excel(input_txt_path, output_excel_path)