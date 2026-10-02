# WSL2 上为 libvmm/QEMU guest 搭建 kind Kubernetes control plane

本文记录如何在 WSL2 中安装 `kind` 和 `kubectl`，创建一个供
`examples/virtio_pci` 中 QEMU guest kubelet 使用的本地 Kubernetes control
plane，并验证 apiserver、etcd、scheduler 和 controller-manager 正常工作。

本文创建的是开发和验证环境，不是生产集群。最终网络拓扑如下：

```text
QEMU client VM
  kubelet
     |
     | HTTPS: https://10.0.2.2:6443
     | virtio-net + QEMU user networking
     v
WSL2
  0.0.0.0:6443
     |
     v
kind control-plane container
  - kube-apiserver
  - etcd
  - kube-controller-manager
  - kube-scheduler
```

## 1. 本文使用的版本

本次验证使用：

```text
Ubuntu on WSL2: 24.04
Docker Engine:  29.5.0
kind:           v0.33.0
kubectl:        v1.37.1
Kubernetes:     v1.37.0
Architecture:   linux/amd64
```

kind v0.33.0 对应的 Kubernetes v1.37.0 node image 是：

```text
kindest/node:v1.37.0@sha256:a1ed56cfb0e7b93589bdf97c8cd566405a265939e3620fc4f5de89adff580ae5
```

这些是可复现的固定版本。将来升级时，应从 kind 官方 release notes 中选择
kind 支持的 node image 和对应 digest，而不是只替换 tag。

相关官方资料：

- [kind Quick Start](https://kind.sigs.k8s.io/docs/user/quick-start/)
- [kind releases](https://github.com/kubernetes-sigs/kind/releases)
- [安装 kubectl](https://kubernetes.io/docs/tasks/tools/install-kubectl-linux/)
- [Kubernetes releases](https://dl.k8s.io/release/stable.txt)

## 2. 前置需求

需要：

- WSL2 Linux 环境；
- 正在运行且当前用户可访问的 Docker daemon；
- `curl`、`sha256sum`、`tar`、`openssl`；
- 足够的磁盘和内存；
- 能访问 GitHub、`dl.k8s.io` 和 Docker Hub，或者已经配置 HTTP(S) 代理。

先检查环境：

```sh
uname -a
uname -m
docker version
df -h /tmp /var/lib/docker
```

当前用户若无法访问 Docker socket，`docker version` 会只显示 client，随后报告
permission denied。普通 Linux 安装通常需要将用户加入 `docker` group，并重新登录：

```sh
sudo usermod -aG docker "$USER"
```

这条命令是否适用取决于 Docker 的安装方式。Docker Desktop、原生 WSL Docker
以及远程 Docker context 的 socket 管理方式可能不同，应先检查：

```sh
docker context inspect
ls -l /var/run/docker.sock
id
```

## 3. 安装 kind

创建用户级安装目录：

```sh
mkdir -p ~/.local/bin
```

下载 kind v0.33.0 及官方 checksum：

```sh
cd /tmp

curl -fLO \
  https://github.com/kubernetes-sigs/kind/releases/download/v0.33.0/kind-linux-amd64

curl -fLO \
  https://github.com/kubernetes-sigs/kind/releases/download/v0.33.0/kind-linux-amd64.sha256sum
```

必须先验证 checksum：

```sh
sha256sum -c kind-linux-amd64.sha256sum
```

期望输出：

```text
kind-linux-amd64: OK
```

然后安装：

```sh
install -m 0755 kind-linux-amd64 ~/.local/bin/kind
kind version
```

期望版本：

```text
kind v0.33.0
```

如果 `kind` 找不到，确保 `~/.local/bin` 位于 `PATH`：

```sh
export PATH="$HOME/.local/bin:$PATH"
```

可以把该行加入 `~/.bashrc`，然后重新打开 shell。

## 4. 安装 kubectl

本文固定使用 v1.37.1：

```sh
cd /tmp

curl -fL -o kubectl \
  https://dl.k8s.io/release/v1.37.1/bin/linux/amd64/kubectl

curl -fL -o kubectl.sha256 \
  https://dl.k8s.io/release/v1.37.1/bin/linux/amd64/kubectl.sha256
```

验证 checksum：

```sh
printf '%s  %s\n' "$(cat kubectl.sha256)" kubectl | sha256sum -c -
```

期望输出：

```text
kubectl: OK
```

只有校验成功后才安装：

```sh
install -m 0755 kubectl ~/.local/bin/kubectl
kubectl version --client
```

查询当前最新稳定版本可以使用：

```sh
curl -fsSL https://dl.k8s.io/release/stable.txt
```

但 kubelet 与 control plane 存在版本偏差约束。为本项目构建 kubelet 时，最好选择
与 kind node image 相同的 Kubernetes minor 版本。

## 5. 创建 kind 配置

创建 `kind-libvmm-poc.yaml`：

```yaml
kind: Cluster
apiVersion: kind.x-k8s.io/v1alpha4
networking:
  apiServerAddress: "0.0.0.0"
  apiServerPort: 6443
nodes:
  - role: control-plane
    kubeadmConfigPatches:
      - |
        kind: ClusterConfiguration
        apiServer:
          certSANs:
            - "10.0.2.2"
            - "127.0.0.1"
```

这里有两个关键设置。

### 5.1 为什么监听 0.0.0.0

若只监听 `127.0.0.1`，QEMU guest 无法访问 apiserver。监听 `0.0.0.0:6443`
让 QEMU user networking 可以把连接交给 WSL2 上的 API server。

这也意味着 apiserver 可能暴露给 WSL2 可以到达的其他网络。该配置只应在受信任的
开发环境使用；不使用时可以删除集群，或者用防火墙限制 6443。

### 5.2 为什么证书包含 10.0.2.2

QEMU user networking 中，guest 通常通过 `10.0.2.2` 访问运行 QEMU 的 host。
guest kubelet 将连接：

```text
https://10.0.2.2:6443
```

因此 apiserver certificate 的 Subject Alternative Name 必须包含 `10.0.2.2`，
否则 kubelet 会报告 x509 IP/SAN 校验失败。

guest 主动连接 apiserver不需要 QEMU `hostfwd`。`hostfwd` 用于 host 主动连接
guest 服务，例如 kubelet 的 10250 端口。

## 6. 创建集群：正常网络路径

确认没有同名集群，且 6443 没有被占用：

```sh
kind get clusters
ss -ltn '( sport = :6443 )'
```

创建名为 `libvmm-poc` 的单 control-plane 集群：

```sh
kind create cluster \
  --name libvmm-poc \
  --config kind-libvmm-poc.yaml \
  --image kindest/node:v1.37.0@sha256:a1ed56cfb0e7b93589bdf97c8cd566405a265939e3620fc4f5de89adff580ae5 \
  --wait 5m
```

kind 会完成：

1. 拉取 node image；
2. 创建 Docker control-plane container；
3. 使用 kubeadm 初始化 Kubernetes；
4. 安装 CNI；
5. 安装默认 StorageClass；
6. 将 kubeconfig context 写入 `~/.kube/config`。

## 7. Docker daemon 无法使用 shell 代理时的 workaround

本次环境中 shell 配置了：

```text
HTTP_PROXY=http://127.0.0.1:1080
HTTPS_PROXY=http://127.0.0.1:1080
```

但 Docker daemon 没有代理配置。因此 `curl` 可以访问网络，而 `docker pull`
直接访问 Docker Hub 时超时：

```text
failed to resolve reference ... dial tcp ...:443: i/o timeout
```

长期解决方案是正确配置 Docker daemon proxy。若不希望修改系统配置，可以使用
`crane` 通过当前 shell 代理下载 OCI image，再导入 Docker。

### 7.1 下载并验证 crane

本次使用 `go-containerregistry` v0.22.1：

```sh
cd /tmp

curl -fL -o go-containerregistry_Linux_x86_64.tar.gz \
  https://github.com/google/go-containerregistry/releases/download/v0.22.1/go-containerregistry_Linux_x86_64.tar.gz

curl -fL -o go-containerregistry-checksums.txt \
  https://github.com/google/go-containerregistry/releases/download/v0.22.1/checksums.txt
```

验证并提取：

```sh
grep 'go-containerregistry_Linux_x86_64.tar.gz$' \
  go-containerregistry-checksums.txt | sha256sum -c -

tar -xzf go-containerregistry_Linux_x86_64.tar.gz crane
./crane version
```

### 7.2 通过代理下载固定 digest 的 kind image

```sh
./crane pull \
  --platform=linux/amd64 \
  kindest/node@sha256:a1ed56cfb0e7b93589bdf97c8cd566405a265939e3620fc4f5de89adff580ae5 \
  /tmp/kindest-node-v1.37.0.tar
```

这里直接按 digest 拉取，registry 返回内容必须与该内容地址匹配。

导入 Docker：

```sh
docker load -i /tmp/kindest-node-v1.37.0.tar
```

crane 按 digest 导出的 Docker archive 可能显示：

```text
Loaded image: kindest/node:i-was-a-digest
```

为本地 image 添加 kind 预期的 tag：

```sh
docker tag kindest/node:i-was-a-digest kindest/node:v1.37.0
docker image inspect kindest/node:v1.37.0
```

此时使用本地 tag 创建集群，避免 Docker daemon 再次 pull：

```sh
kind create cluster \
  --name libvmm-poc \
  --config kind-libvmm-poc.yaml \
  --image kindest/node:v1.37.0 \
  --wait 5m
```

## 8. 修正本机 kubectl endpoint

由于 `apiServerAddress` 是 `0.0.0.0`，kind 可能在生成的 kubeconfig 中写入：

```yaml
server: https://0.0.0.0:6443
```

`0.0.0.0` 适合作为监听地址，不适合作为客户端目标地址；在存在 HTTP proxy 时还可能
被错误发送给代理。本机 kubectl 应使用 `127.0.0.1`：

```sh
kubectl config set-cluster kind-libvmm-poc \
  --server=https://127.0.0.1:6443
```

这不会改变 apiserver 的监听地址。QEMU guest 的专用 kubeconfig 仍应使用：

```text
https://10.0.2.2:6443
```

## 9. 验证 control plane

检查当前 context：

```sh
kubectl config current-context
```

期望：

```text
kind-libvmm-poc
```

检查 control plane：

```sh
kubectl cluster-info --context kind-libvmm-poc
kubectl get nodes -o wide
kubectl get pods -A
```

Node 应为 `Ready`，以下组件应为 `Running`：

```text
etcd-libvmm-poc-control-plane
kube-apiserver-libvmm-poc-control-plane
kube-controller-manager-libvmm-poc-control-plane
kube-scheduler-libvmm-poc-control-plane
coredns
kindnet
kube-proxy
```

检查 Docker container 和端口：

```sh
docker ps --filter name=libvmm-poc
ss -ltn '( sport = :6443 )'
```

期望看到：

```text
0.0.0.0:6443->6443/tcp
```

检查 TLS certificate SAN：

```sh
openssl s_client \
  -connect 127.0.0.1:6443 \
  -servername 10.0.2.2 \
  </dev/null 2>/dev/null |
  openssl x509 -noout -ext subjectAltName
```

输出应包含：

```text
IP Address:10.0.2.2
IP Address:127.0.0.1
```

## 10. kubectl 状态读写 smoke test

创建一个独立 namespace 和 ConfigMap，不需要额外拉取容器镜像：

```sh
kubectl create namespace libvmm-poc

kubectl create configmap control-plane-smoke-test \
  --from-literal=message=hello-from-libvmm \
  --namespace libvmm-poc
```

读回对象：

```sh
kubectl get configmap control-plane-smoke-test \
  --namespace libvmm-poc \
  -o yaml
```

期望看到：

```yaml
data:
  message: hello-from-libvmm
```

这验证了：

```text
kubectl -> kube-apiserver -> etcd -> kube-apiserver -> kubectl
```

## 11. 常用管理命令

列出 kind 集群：

```sh
kind get clusters
```

查看集群状态：

```sh
kubectl get nodes
kubectl get pods -A
kubectl get events -A --sort-by=.metadata.creationTimestamp
```

查看 control-plane container 日志：

```sh
docker logs libvmm-poc-control-plane
```

导出 kind 诊断信息：

```sh
kind export logs --name libvmm-poc /tmp/libvmm-poc-logs
```

停止并重新启动 control-plane container：

```sh
docker stop libvmm-poc-control-plane
docker start libvmm-poc-control-plane
```

## 12. 清理

删除测试对象但保留集群：

```sh
kubectl delete namespace libvmm-poc
```

删除整个 kind 集群：

```sh
kind delete cluster --name libvmm-poc
```

这会删除 kind control-plane container 和该集群对应的 Docker network/state，
但不会自动删除下载的 node image。

需要时可以另外检查镜像：

```sh
docker image ls kindest/node
```

不要在仍有其他 kind 集群使用该镜像时删除它。

## 13. 下一阶段：接入 QEMU guest kubelet

control plane 就绪后，后续工作是：

1. 从 Kubernetes v1.37.x 源码构建 `linux/arm64` kubelet；
2. 为 `system:node:sel4-worker` 创建最小权限身份；
3. 生成 guest 专用 kubeconfig，server 使用 `https://10.0.2.2:6443`；
4. 将 kubelet、CA、client certificate 和配置打包进 guest rootfs；
5. 在 guest 中挂载最小 cgroup v2 filesystem；
6. 实现最小 CRI v1 gRPC endpoint；
7. 先让 `sel4-worker` 以 `NotReady` 状态注册，再逐步实现 runtime 功能。

注意：不要把 kind 的 cluster-admin kubeconfig 直接打包进 VM。应为 guest kubelet
创建专用的 Node 身份，避免 VM 获得整个集群的管理员权限。
