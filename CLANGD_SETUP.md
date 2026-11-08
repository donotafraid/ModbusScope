# Clangd 配置指南

## 概述
Clangd 是一个基于 LLVM 的语言服务器，为 C++ 项目提供强大的代码补全、错误检查、重构等功能。

## 当前配置

### 1. VS Code 设置 (.vscode/settings.json)
```json
{
  "clangd.path": "/usr/bin/clangd",
  "clangd.arguments": [
    "--background-index",
    "--compile-commands-dir=${workspaceFolder}",
    "--query-driver=/usr/bin/g++",
    "--log=error"
  ],
  "debugger": "gdb",
  "cmake.configureSettings": {
    "CMAKE_C_COMPILER": "gcc",
    "CMAKE_CXX_COMPILER": "g++",
    "CMAKE_BUILD_TYPE": "Debug"
  },
  "C_Cpp.intelliSenseEngine": "disabled",
  "C_Cpp.autocomplete": "disabled",
  "C_Cpp.errorSquiggles": "disabled",
  "C_Cpp.formatting": "disabled",
  "C_Cpp.autoAddFileAssociations": false,
  "C_Cpp.default.cppStandard": "gnu++17",
  "C_Cpp.default.cStandard": "gnu17",
  "files.associations": {
    "*.h": "cpp",
    "*.hpp": "cpp",
    "*.cpp": "cpp",
    "*.cc": "cpp",
    "*.cxx": "cpp"
  }
}
```

### 2. Clangd 配置文件 (.clangd)
```yaml
CompileFlags:
  Add: 
    - -isystem
    - /usr/include/c++/11
    - -isystem
    - /usr/include/x86_64-linux-gnu/c++/11
    - -isystem
    - /usr/include/c++/11/backward
    - -isystem
    - /usr/lib/gcc/x86_64-linux-gnu/11/include
    - -isystem
    - /usr/local/include
    - -isystem
    - /usr/include/x86_64-linux-gnu
    - -isystem
    - /usr/include
    - -std=gnu++17
  CompilationDatabase: compile_commands.json

Index:
  Background: Build
```

### 3. CMakeLists.txt 配置
确保在 CMakeLists.txt 中正确配置系统包含路径：
```cmake
include_directories(
    "${CurrentFolder}/include"
)

# 添加系统包含目录
include_directories(SYSTEM
    "/usr/include"
    "/usr/include/c++/11"
    "/usr/include/x86_64-linux-gnu/c++/11"
    "/usr/include/c++/11/backward"
    "/usr/lib/gcc/x86_64-linux-gnu/11/include"
    "/usr/local/include"
    "/usr/include/x86_64-linux-gnu"
)
```

## 功能特性

### 已启用的功能：
1. **后台索引** - 项目代码在后台被索引，提供更好的代码补全
2. **编译命令数据库** - 使用 CMake 生成的 compile_commands.json
3. **智能感知禁用** - 禁用 VS Code 内置的 C++ 扩展，避免冲突
4. **系统包含路径** - 正确配置标准库路径，避免内置函数定义冲突

### 可用的功能：
- 代码补全
- 错误检查和诊断
- 跳转到定义
- 查找引用
- 符号重命名
- 代码格式化
- 悬停信息显示

## 使用方法

1. **重启 VS Code** - 确保新配置生效
2. **打开 C++ 文件** - clangd 会自动开始工作
3. **等待索引完成** - 首次打开项目时，后台索引需要一些时间

## 故障排除

### 常见问题及解决方案：

#### 1. 'iostream' file not found
**原因**: 缺少标准库包含路径
**解决方案**: 
- 确保 `.clangd` 文件中包含正确的 `-isystem` 路径
- 重新生成 `compile_commands.json`

#### 2. definition of builtin function '_mm_getcsr'
**原因**: 系统包含路径配置不正确
**解决方案**:
- 使用 `-isystem` 而不是 `-I` 来包含系统头文件
- 确保包含路径顺序正确

#### 3. clangd 服务器崩溃
**原因**: 配置参数错误
**解决方案**:
- 简化 clangd 参数
- 移除过时的参数
- 检查日志级别设置

### 如果 clangd 不工作：
1. 检查 `compile_commands.json` 是否存在且有效
2. 确保 clangd 已安装：`which clangd`
3. 查看 VS Code 的输出面板中的 clangd 日志
4. 重新生成 compile_commands.json：`cmake .. -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`

## 验证配置

运行以下命令验证 clangd 是否正常工作：
```bash
clangd --version
clangd --background-index --compile-commands-dir=. --log=error
```

## 高级配置

如果需要更多功能，可以在 `.clangd` 文件中添加：

```yaml
CompileFlags:
  Add: 
    - -isystem
    - /usr/include/c++/11
    - -isystem
    - /usr/include/x86_64-linux-gnu/c++/11
    - -isystem
    - /usr/include/c++/11/backward
    - -isystem
    - /usr/lib/gcc/x86_64-linux-gnu/11/include
    - -isystem
    - /usr/local/include
    - -isystem
    - /usr/include/x86_64-linux-gnu
    - -isystem
    - /usr/include
    - -std=gnu++17
  CompilationDatabase: compile_commands.json

Diagnostics:
  ClangTidy:
    Add: 
      - modernize*
      - performance*
      - readability*
    Remove:
      - modernize-use-trailing-return-type
      - cppcoreguidelines-avoid-magic-numbers

Index:
  Background: Build

InlayHints:
  Enabled: Yes
  ParameterNames: Yes
  DeducedTypes: Yes
```

## 最新修复

### 2024-07-26 修复的问题：
1. ✅ 修复了 `'iostream' file not found` 错误
2. ✅ 修复了 `definition of builtin function '_mm_getcsr'` 错误
3. ✅ 优化了系统包含路径配置
4. ✅ 简化了 clangd 参数配置
5. ✅ 更新了 CMakeLists.txt 配置 