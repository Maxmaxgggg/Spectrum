#!/bin/bash
# Ставит всё, что нужно проверочной сборке под Linux: Qt 5, GMP, clang
# (устройство CUDA — через него, nvcc не нужен), clang-tidy и заголовки,
# ptxas и libcudart из колёс pip NVIDIA — в $CUDA_ROOT (по умолчанию
# /tmp/cuda). Видеокарта не нужна: ядра компилируются до машинного кода
# (ptxas), но не запускаются.
set -e
CUDA=${CUDA_ROOT:-/tmp/cuda}
VENV=${CUDA_VENV:-/tmp/cudaenv}

if ! dpkg -s qtbase5-dev libgmp-dev clang clang-tidy rsync >/dev/null 2>&1; then
    apt-get update -q >/dev/null
    apt-get install -y -q qtbase5-dev qtbase5-dev-tools libgmp-dev clang clang-tidy rsync \
        python3-venv pkg-config >/dev/null
fi

if [ ! -x "$CUDA/bin/ptxas" ]; then
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install -q "nvidia-cuda-runtime-cu12==12.9.*" "nvidia-cuda-nvcc-cu12==12.9.*" \
        "nvidia-cuda-cccl-cu12==12.9.*" "nvidia-curand-cu12"
    R=$(echo "$VENV"/lib/python3*/site-packages/nvidia)
    rm -rf "$CUDA"
    mkdir -p "$CUDA/include" "$CUDA/bin" "$CUDA/lib64" "$CUDA/nvvm/libdevice"
    # curand — только ради заголовков: их подключает обёртка CUDA у clang.
    for p in cuda_runtime cuda_nvcc cuda_cccl curand; do
        [ -d "$R/$p/include" ] && cp -r "$R/$p/include/." "$CUDA/include/"
    done
    cp "$R/cuda_nvcc/bin/ptxas" "$CUDA/bin/"
    cp "$R/cuda_nvcc/nvvm/libdevice/libdevice.10.bc" "$CUDA/nvvm/libdevice/"
    cp "$R/cuda_runtime/lib/libcudart.so.12" "$CUDA/lib64/"
    ln -sf libcudart.so.12 "$CUDA/lib64/libcudart.so"
    echo '{"cuda":{"name":"CUDA SDK","version":"12.9.1"}}' > "$CUDA/version.json"
    echo "CUDA Version 12.9.1" > "$CUDA/version.txt"
fi
echo "готово: Qt $(pkg-config --modversion Qt5Core), $(clang++ --version | head -1), CUDA в $CUDA"
