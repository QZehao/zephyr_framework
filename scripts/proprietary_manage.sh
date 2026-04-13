#!/usr/bin/env bash
# =============================================================================
# 商业模块管理脚本 (Bash)
# =============================================================================
# 用途：管理商业模块子模块的启用/禁用/状态查看
#
# 使用方法：
#   ./proprietary_manage.sh enable          # 启用商业模块
#   ./proprietary_manage.sh disable         # 禁用商业模块
#   ./proprietary_manage.sh status          # 查看状态
#   ./proprietary_manage.sh update          # 更新商业模块
# =============================================================================

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# 输出函数
info() { echo -e "${CYAN}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[OK]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# 项目目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
PROPRIETARY_DIR="$PROJECT_ROOT/src/proprietary"
PROPRIETARY_COMMON_CMAKE="$PROPRIETARY_DIR/proprietary_modules_common.cmake"

show_help() {
    cat << EOF
商业模块管理脚本

用法:
    ./proprietary_manage.sh enable      启用商业模块（初始化子模块）
    ./proprietary_manage.sh disable     禁用商业模块（移除子模块内容）
    ./proprietary_manage.sh status      查看商业模块状态
    ./proprietary_manage.sh update      更新商业模块到最新版本
    ./proprietary_manage.sh help        显示帮助信息

说明:
    商业模块目录 (src/proprietary) 是一个 Git 子模块。
    - 启用后：编译时可以配置并使用商业模块功能
    - 禁用后：工程仍然可以正常编译，但无法使用商业模块

EOF
}

check_git() {
    if ! command -v git &> /dev/null; then
        error "Git 未安装或不在 PATH 中"
    fi
}

is_git_repo() {
    check_git
    git -C "$PROJECT_ROOT" rev-parse --git-dir &> /dev/null
}

get_submodule_status() {
    check_git
    git -C "$PROJECT_ROOT" submodule status -- "src/proprietary" 2>/dev/null || echo ""
}

enable_proprietary() {
    info "正在启用商业模块..."

    if ! is_git_repo; then
        error "当前目录不是 Git 仓库"
    fi

    cd "$PROJECT_ROOT"

    # 检查子模块是否已配置
    if ! git config --file .gitmodules --get "submodule.src/proprietary.url" &> /dev/null; then
        error "商业模块子模块未配置，请检查 .gitmodules 文件"
    fi

    # 初始化并更新子模块
    info "初始化子模块..."
    if git submodule update --init --recursive -- "src/proprietary"; then
        :
    else
        error "子模块初始化失败"
    fi

    # 验证
    if [[ -f "$PROPRIETARY_COMMON_CMAKE" ]]; then
        success "商业模块已启用"
        info "商业模块目录: $PROPRIETARY_DIR"

        # 列出可用的商业模块
        echo ""
        info "可用的商业模块:"
        find "$PROPRIETARY_DIR" -maxdepth 2 -name "Kconfig" -exec dirname {} \; | while read -r module_dir; do
            module_name=$(basename "$module_dir")
            echo "  - $module_name"
        done
    else
        warn "商业模块目录存在但文件不完整"
    fi
}

disable_proprietary() {
    info "正在禁用商业模块..."

    if ! is_git_repo; then
        error "当前目录不是 Git 仓库"
    fi

    cd "$PROJECT_ROOT"

    # 检查目录是否存在
    if [[ ! -d "$PROPRIETARY_DIR" ]]; then
        warn "商业模块目录不存在，无需禁用"
        return 0
    fi

    # 检查是否有未提交的更改
    if [[ -d "$PROPRIETARY_DIR/.git" ]]; then
        cd "$PROPRIETARY_DIR"
        status=$(git status --porcelain 2>/dev/null || echo "")
        if [[ -n "$status" ]]; then
            warn "商业模块目录有未提交的更改:"
            echo "$status"
            read -p "是否继续禁用？ (y/N) " -n 1 -r
            echo
            if [[ ! $REPLY =~ ^[Yy]$ ]]; then
                info "操作已取消"
                return 0
            fi
        fi
        cd "$PROJECT_ROOT"
    fi

    # 移除子模块内容
    info "移除商业模块内容..."
    if git submodule deinit -f -- "src/proprietary" 2>/dev/null; then
        success "商业模块已禁用"
        info "工程现在可以在不包含商业模块的情况下编译"
        info "如需重新启用，请运行: ./proprietary_manage.sh enable"
    else
        # 如果 deinit 失败，尝试手动移除
        warn "使用备用方法移除..."
        rm -rf "$PROPRIETARY_DIR"
        success "商业模块已禁用"
    fi
}

show_status() {
    info "商业模块状态:"
    echo ""

    # 检查目录是否存在
    if [[ ! -d "$PROPRIETARY_DIR" ]]; then
        warn "商业模块目录不存在 (src/proprietary)"
        info "状态: 未启用"
        info "运行 './proprietary_manage.sh enable' 来启用"
        return 0
    fi

    # 检查关键文件是否存在
    if [[ ! -f "$PROPRIETARY_COMMON_CMAKE" ]]; then
        warn "商业模块目录存在但内容不完整"
        info "状态: 需要更新"
        info "运行 './proprietary_manage.sh update' 来修复"
        return 0
    fi

    success "商业模块目录: $PROPRIETARY_DIR"
    echo ""

    # 获取子模块状态
    submodule_status=$(get_submodule_status)
    if [[ -n "$submodule_status" ]]; then
        status_char="${submodule_status:0:1}"
        case "$status_char" in
            ' ') info "Git 状态: 已初始化" ;;
            '-') warn "Git 状态: 未初始化" ;;
            '+') warn "Git 状态: 有更新可用" ;;
            *) info "Git 状态: $submodule_status" ;;
        esac
    fi

    echo ""
    info "可用的商业模块:"
    echo ""

    find "$PROPRIETARY_DIR" -maxdepth 2 -name "Kconfig" -exec dirname {} \; | sort | while read -r module_dir; do
        module_name=$(basename "$module_dir")
        is_submodule=""

        if [[ -d "$module_dir/.git" ]]; then
            is_submodule=" [子模块]"
        fi

        echo "  $module_name$is_submodule"

        # 尝试读取模块描述
        kconfig_file="$module_dir/Kconfig"
        if [[ -f "$kconfig_file" ]]; then
            desc=$(grep -m1 'menu "' "$kconfig_file" | sed 's/menu "\([^"]*\)"/\1/' || true)
            if [[ -n "$desc" ]]; then
                echo -e "    描述: ${desc}"
            fi
        fi
    done
}

update_proprietary() {
    info "正在更新商业模块..."

    if ! is_git_repo; then
        error "当前目录不是 Git 仓库"
    fi

    cd "$PROJECT_ROOT"

    # 更新主子模块
    info "更新主商业模块..."
    if ! git submodule update --init --recursive --remote -- "src/proprietary"; then
        error "更新失败"
    fi

    # 更新内部子模块
    if [[ -d "$PROPRIETARY_DIR" ]]; then
        cd "$PROPRIETARY_DIR"
        if [[ -f ".gitmodules" ]]; then
            info "更新内部子模块..."
            git submodule update --init --recursive --remote || true
        fi
    fi

    success "商业模块已更新"
}

# 主逻辑
case "${1:-help}" in
    enable)
        enable_proprietary
        ;;
    disable)
        disable_proprietary
        ;;
    status)
        show_status
        ;;
    update)
        update_proprietary
        ;;
    help|--help|-h)
        show_help
        ;;
    *)
        error "未知命令: $1\n运行 './proprietary_manage.sh help' 查看帮助"
        ;;
esac
