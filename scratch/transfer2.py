import pandas as pd
import sys

def tsv_to_xlsx(input_file, output_file):
    """
    将制表符分隔的文本文件 (TSV) 转换为 Excel 文件 (XLSX)。
    """
    try:
        # 1. 使用制表符作为分隔符读取文本文件
        # header=0 表示使用第一行作为列名
        df = pd.read_csv(input_file, sep='\t', header=0)

        # 2. 将 DataFrame 写入 Excel 文件
        # index=False 表示不将 DataFrame 的索引（默认的行号）写入 Excel
        df.to_excel(output_file, index=False)
        
        print(f"转换成功！文件已保存为: {output_file}")
        
    except FileNotFoundError:
        print(f"错误: 找不到输入文件 {input_file}。请检查文件名和路径是否正确。")
    except Exception as e:
        print(f"发生错误: {e}")

if __name__ == "__main__":
    # 请替换为您实际的追踪日志文件名
    # 假设您的文件名为 congestion_trace_cam_10.txt
    
    # 示例用法：
    # 如果您直接运行脚本，它将使用默认文件名
    input_file_name = "scratch/congestion_trace_cam_10.txt"  
    output_file_name = "scratch/congestion_trace.xlsx"
    
    # 如果用户通过命令行提供了文件名，则使用命令行参数
    if len(sys.argv) > 1:
        input_file_name = sys.argv[1]
    
    tsv_to_xlsx(input_file_name, output_file_name)