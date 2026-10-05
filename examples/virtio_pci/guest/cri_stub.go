// Copyright 2026
// SPDX-License-Identifier: BSD-2-Clause
// CRI v1 to the host-native lifecycle protocol over AF_VSOCK.
package main

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"time"

	"golang.org/x/sys/unix"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	runtimeapi "k8s.io/cri-api/pkg/apis/runtime/v1"
)

const (
	socketPath        = "/run/cri-vsock.sock"
	rpcMagic          = 0x4352564d
	rpcVersion        = 1
	hostCID           = 2
	hostPort          = 1234
	opHello           = 1
	opRunSandbox      = 10
	opStopSandbox     = 11
	opRemoveSandbox   = 12
	opSandboxStatus   = 13
	opListSandboxes   = 14
	opCreateContainer = 20
	opStartContainer  = 21
	opStopContainer   = 22
	opRemoveContainer = 23
	opContainerStatus = 24
	opListContainers  = 25
	opReopenLog       = 26
	opPullImage       = 30
	opRemoveImage     = 31
	opImageStatus     = 32
	opListImages      = 33
	rpcOK             = 0
	rpcNotFound       = 1
	rpcInvalid        = 2
	rpcFull           = 3
)

type wireObject struct {
	Cursor, Present, Attempt, State, ExitCode                               uint32
	NetworkNS, IPCNS, PIDNS                                                 uint32
	CreatedAt, StartedAt, FinishedAt                                        uint64
	ID, Name, Namespace, UID                                                string
	Labels, Annotations                                                     map[string]string
	RuntimeHandler, LogDir, SandboxID, Image, ImageRef, LogPath, Reason, IP string
}

var fieldSizes = []int{64, 128, 64, 64, 512, 512, 64, 192, 64, 256, 256, 192, 64, 48}

func putFixed(dst []byte, s string) {
	if len(dst) > 0 {
		copy(dst[:len(dst)-1], []byte(s))
	}
}
func getFixed(src []byte) string {
	for i, b := range src {
		if b == 0 {
			return string(src[:i])
		}
	}
	return string(src)
}
func encodeMap(m map[string]string, dst []byte) {
	p := 2
	count := 0
	for k, v := range m {
		if len(k) > 65535 || len(v) > 65535 || p+4+len(k)+len(v) > len(dst) {
			continue
		}
		binary.LittleEndian.PutUint16(dst[p:], uint16(len(k)))
		binary.LittleEndian.PutUint16(dst[p+2:], uint16(len(v)))
		p += 4
		copy(dst[p:], k)
		p += len(k)
		copy(dst[p:], v)
		p += len(v)
		count++
	}
	binary.LittleEndian.PutUint16(dst, uint16(count))
}
func decodeMap(src []byte) map[string]string {
	m := map[string]string{}
	if len(src) < 2 {
		return m
	}
	n, p := int(binary.LittleEndian.Uint16(src)), 2
	for i := 0; i < n && p+4 <= len(src); i++ {
		kl := int(binary.LittleEndian.Uint16(src[p:]))
		vl := int(binary.LittleEndian.Uint16(src[p+2:]))
		p += 4
		if p+kl+vl > len(src) {
			break
		}
		k := string(src[p : p+kl])
		p += kl
		m[k] = string(src[p : p+vl])
		p += vl
	}
	return m
}

func (o *wireObject) marshal() []byte {
	b := make([]byte, 2536)
	p := 0
	for _, v := range []uint32{o.Cursor, o.Present, o.Attempt, o.State, o.ExitCode} {
		binary.LittleEndian.PutUint32(b[p:], v)
		p += 4
	}
	for _, v := range []uint32{o.NetworkNS, o.IPCNS, o.PIDNS} {
		binary.LittleEndian.PutUint32(b[p:], v)
		p += 4
	}
	for _, v := range []uint64{o.CreatedAt, o.StartedAt, o.FinishedAt} {
		binary.LittleEndian.PutUint64(b[p:], v)
		p += 8
	}
	strings := []string{o.ID, o.Name, o.Namespace, o.UID}
	for i, s := range strings {
		putFixed(b[p:p+fieldSizes[i]], s)
		p += fieldSizes[i]
	}
	encodeMap(o.Labels, b[p:p+512])
	p += 512
	encodeMap(o.Annotations, b[p:p+512])
	p += 512
	strings = []string{o.RuntimeHandler, o.LogDir, o.SandboxID, o.Image, o.ImageRef, o.LogPath, o.Reason, o.IP}
	for i, s := range strings {
		n := fieldSizes[i+6]
		putFixed(b[p:p+n], s)
		p += n
	}
	return b
}
func unmarshalObject(b []byte) wireObject {
	var o wireObject
	p := 0
	nums := []*uint32{&o.Cursor, &o.Present, &o.Attempt, &o.State, &o.ExitCode}
	for _, v := range nums {
		*v = binary.LittleEndian.Uint32(b[p:])
		p += 4
	}
	for _, v := range []*uint32{&o.NetworkNS, &o.IPCNS, &o.PIDNS} {
		*v = binary.LittleEndian.Uint32(b[p:])
		p += 4
	}
	times := []*uint64{&o.CreatedAt, &o.StartedAt, &o.FinishedAt}
	for _, v := range times {
		*v = binary.LittleEndian.Uint64(b[p:])
		p += 8
	}
	ss := []*string{&o.ID, &o.Name, &o.Namespace, &o.UID}
	for i, v := range ss {
		n := fieldSizes[i]
		*v = getFixed(b[p : p+n])
		p += n
	}
	o.Labels = decodeMap(b[p : p+512])
	p += 512
	o.Annotations = decodeMap(b[p : p+512])
	p += 512
	ss = []*string{&o.RuntimeHandler, &o.LogDir, &o.SandboxID, &o.Image, &o.ImageRef, &o.LogPath, &o.Reason, &o.IP}
	for i, v := range ss {
		n := fieldSizes[i+6]
		*v = getFixed(b[p : p+n])
		p += n
	}
	return o
}

type hostClient struct {
	mu   sync.Mutex
	fd   int
	next atomic.Uint32
}

func (c *hostClient) close() {
	if c.fd >= 0 {
		_ = unix.Close(c.fd)
		c.fd = -1
	}
}
func (c *hostClient) connect() error {
	if c.fd >= 0 {
		return nil
	}
	fd, e := unix.Socket(unix.AF_VSOCK, unix.SOCK_STREAM, 0)
	if e != nil {
		return e
	}
	if e = unix.Connect(fd, &unix.SockaddrVM{CID: hostCID, Port: hostPort}); e != nil {
		unix.Close(fd)
		return e
	}
	c.fd = fd
	return nil
}
func fdWrite(fd int, b []byte) error {
	for len(b) > 0 {
		n, e := unix.Write(fd, b)
		if e == unix.EINTR {
			continue
		}
		if e != nil {
			return e
		}
		if n == 0 {
			return io.ErrUnexpectedEOF
		}
		b = b[n:]
	}
	return nil
}
func fdRead(fd int, b []byte) error {
	for len(b) > 0 {
		n, e := unix.Read(fd, b)
		if e == unix.EINTR {
			continue
		}
		if e != nil {
			return e
		}
		if n == 0 {
			return io.EOF
		}
		b = b[n:]
	}
	return nil
}
func (c *hostClient) call(op uint16, req wireObject) (wireObject, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if e := c.connect(); e != nil {
		return wireObject{}, status.Errorf(codes.Unavailable, "host runtime connect: %v", e)
	}
	body := req.marshal()
	h := make([]byte, 20)
	binary.LittleEndian.PutUint32(h, rpcMagic)
	binary.LittleEndian.PutUint16(h[4:], rpcVersion)
	binary.LittleEndian.PutUint16(h[6:], op)
	binary.LittleEndian.PutUint32(h[8:], c.next.Add(1))
	binary.LittleEndian.PutUint32(h[16:], uint32(len(body)))
	e := fdWrite(c.fd, h)
	if e == nil {
		e = fdWrite(c.fd, body)
	}
	if e == nil {
		e = fdRead(c.fd, h)
	}
	if e != nil {
		c.close()
		return wireObject{}, status.Errorf(codes.Unavailable, "host runtime transport: %v", e)
	}
	if binary.LittleEndian.Uint32(h) != rpcMagic || binary.LittleEndian.Uint32(h[16:]) != uint32(len(body)) {
		c.close()
		return wireObject{}, status.Error(codes.Internal, "invalid host runtime response")
	}
	if e := fdRead(c.fd, body); e != nil {
		c.close()
		return wireObject{}, status.Errorf(codes.Unavailable, "host runtime response: %v", e)
	}
	s := binary.LittleEndian.Uint32(h[12:])
	if s != rpcOK {
		if s == rpcNotFound {
			return wireObject{}, status.Error(codes.NotFound, "host object not found")
		}
		if s == rpcFull {
			return wireObject{}, status.Error(codes.ResourceExhausted, "host object table full")
		}
		return wireObject{}, status.Error(codes.InvalidArgument, "host rejected request")
	}
	return unmarshalObject(body), nil
}
func (c *hostClient) list(op uint16) ([]wireObject, error) {
	var out []wireObject
	var cursor uint32
	for cursor < 32 {
		o, e := c.call(op, wireObject{Cursor: cursor})
		if e != nil {
			return nil, e
		}
		if o.Present == 0 {
			break
		}
		out = append(out, o)
		if o.Cursor <= cursor {
			break
		}
		cursor = o.Cursor
	}
	return out, nil
}

type runtimeServer struct {
	runtimeapi.UnimplementedRuntimeServiceServer
	host *hostClient
}
type imageServer struct {
	runtimeapi.UnimplementedImageServiceServer
	host *hostClient
}

func meta(m *runtimeapi.PodSandboxMetadata) (string, string, string, uint32) {
	return m.GetName(), m.GetNamespace(), m.GetUid(), m.GetAttempt()
}
func cmeta(m *runtimeapi.ContainerMetadata) (string, uint32) { return m.GetName(), m.GetAttempt() }
func labelsMatch(have, want map[string]string) bool {
	for k, v := range want {
		if have[k] != v {
			return false
		}
	}
	return true
}

func (s *runtimeServer) Version(_ context.Context, r *runtimeapi.VersionRequest) (*runtimeapi.VersionResponse, error) {
	return &runtimeapi.VersionResponse{Version: r.Version, RuntimeName: "libvmm-host-runtime", RuntimeVersion: "0.2.0", RuntimeApiVersion: "v1"}, nil
}
func (s *runtimeServer) Status(_ context.Context, _ *runtimeapi.StatusRequest) (*runtimeapi.StatusResponse, error) {
	if _, e := s.host.call(opHello, wireObject{}); e != nil {
		return nil, e
	}
	return &runtimeapi.StatusResponse{Status: &runtimeapi.RuntimeStatus{Conditions: []*runtimeapi.RuntimeCondition{{Type: "RuntimeReady", Status: true, Reason: "HostRuntimeReady"}, {Type: "NetworkReady", Status: true, Reason: "DummyNetworkReady"}}}}, nil
}
func (*runtimeServer) RuntimeConfig(context.Context, *runtimeapi.RuntimeConfigRequest) (*runtimeapi.RuntimeConfigResponse, error) {
	return &runtimeapi.RuntimeConfigResponse{Linux: &runtimeapi.LinuxRuntimeConfiguration{CgroupDriver: runtimeapi.CgroupDriver_CGROUPFS}}, nil
}
func (*runtimeServer) UpdateRuntimeConfig(context.Context, *runtimeapi.UpdateRuntimeConfigRequest) (*runtimeapi.UpdateRuntimeConfigResponse, error) {
	return &runtimeapi.UpdateRuntimeConfigResponse{}, nil
}
func (s *runtimeServer) RunPodSandbox(_ context.Context, r *runtimeapi.RunPodSandboxRequest) (*runtimeapi.RunPodSandboxResponse, error) {
	n, ns, uid, a := meta(r.GetConfig().GetMetadata())
	nso := r.GetConfig().GetLinux().GetSecurityContext().GetNamespaceOptions()
	o, e := s.host.call(opRunSandbox, wireObject{Name: n, Namespace: ns, UID: uid, Attempt: a, NetworkNS: uint32(nso.GetNetwork()), IPCNS: uint32(nso.GetIpc()), PIDNS: uint32(nso.GetPid()), Labels: r.GetConfig().GetLabels(), Annotations: r.GetConfig().GetAnnotations(), RuntimeHandler: r.GetRuntimeHandler(), LogDir: r.GetConfig().GetLogDirectory()})
	if e != nil {
		return nil, e
	}
	return &runtimeapi.RunPodSandboxResponse{PodSandboxId: o.ID}, nil
}
func (s *runtimeServer) StopPodSandbox(_ context.Context, r *runtimeapi.StopPodSandboxRequest) (*runtimeapi.StopPodSandboxResponse, error) {
	_, e := s.host.call(opStopSandbox, wireObject{ID: r.GetPodSandboxId()})
	return &runtimeapi.StopPodSandboxResponse{}, e
}
func (s *runtimeServer) RemovePodSandbox(_ context.Context, r *runtimeapi.RemovePodSandboxRequest) (*runtimeapi.RemovePodSandboxResponse, error) {
	_, e := s.host.call(opRemoveSandbox, wireObject{ID: r.GetPodSandboxId()})
	return &runtimeapi.RemovePodSandboxResponse{}, e
}
func sandboxStatus(o wireObject) *runtimeapi.PodSandboxStatus {
	nso := &runtimeapi.NamespaceOption{Network: runtimeapi.NamespaceMode(o.NetworkNS), Ipc: runtimeapi.NamespaceMode(o.IPCNS), Pid: runtimeapi.NamespaceMode(o.PIDNS)}
	return &runtimeapi.PodSandboxStatus{Id: o.ID, Metadata: &runtimeapi.PodSandboxMetadata{Name: o.Name, Namespace: o.Namespace, Uid: o.UID, Attempt: o.Attempt}, State: runtimeapi.PodSandboxState(o.State), CreatedAt: int64(o.CreatedAt), Network: &runtimeapi.PodSandboxNetworkStatus{Ip: o.IP}, Linux: &runtimeapi.LinuxPodSandboxStatus{Namespaces: &runtimeapi.Namespace{Options: nso}}, Labels: o.Labels, Annotations: o.Annotations, RuntimeHandler: o.RuntimeHandler}
}
func (s *runtimeServer) PodSandboxStatus(_ context.Context, r *runtimeapi.PodSandboxStatusRequest) (*runtimeapi.PodSandboxStatusResponse, error) {
	o, e := s.host.call(opSandboxStatus, wireObject{ID: r.GetPodSandboxId()})
	if e != nil {
		return nil, e
	}
	return &runtimeapi.PodSandboxStatusResponse{Status: sandboxStatus(o)}, nil
}
func (s *runtimeServer) ListPodSandbox(_ context.Context, r *runtimeapi.ListPodSandboxRequest) (*runtimeapi.ListPodSandboxResponse, error) {
	os, e := s.host.list(opListSandboxes)
	if e != nil {
		return nil, e
	}
	f := r.GetFilter()
	out := []*runtimeapi.PodSandbox{}
	for _, o := range os {
		if f != nil && (f.GetId() != "" && f.GetId() != o.ID || f.GetState() != nil && uint32(f.GetState().GetState()) != o.State || !labelsMatch(o.Labels, f.GetLabelSelector())) {
			continue
		}
		st := sandboxStatus(o)
		out = append(out, &runtimeapi.PodSandbox{Id: st.Id, Metadata: st.Metadata, State: st.State, CreatedAt: st.CreatedAt, Labels: st.Labels, Annotations: st.Annotations, RuntimeHandler: st.RuntimeHandler})
	}
	return &runtimeapi.ListPodSandboxResponse{Items: out}, nil
}
func (s *runtimeServer) CreateContainer(_ context.Context, r *runtimeapi.CreateContainerRequest) (*runtimeapi.CreateContainerResponse, error) {
	n, a := cmeta(r.GetConfig().GetMetadata())
	sb, e := s.host.call(opSandboxStatus, wireObject{ID: r.GetPodSandboxId()})
	if e != nil {
		return nil, e
	}
	lp := filepath.Join(sb.LogDir, r.GetConfig().GetLogPath())
	if e = os.MkdirAll(filepath.Dir(lp), 0755); e != nil {
		return nil, status.Errorf(codes.Internal, "create log dir: %v", e)
	}
	f, e := os.OpenFile(lp, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if e != nil {
		return nil, status.Errorf(codes.Internal, "create log: %v", e)
	}
	f.Close()
	o, e := s.host.call(opCreateContainer, wireObject{Name: n, Attempt: a, SandboxID: r.GetPodSandboxId(), Image: r.GetConfig().GetImage().GetImage(), Labels: r.GetConfig().GetLabels(), Annotations: r.GetConfig().GetAnnotations(), LogPath: lp})
	if e != nil {
		return nil, e
	}
	return &runtimeapi.CreateContainerResponse{ContainerId: o.ID}, nil
}
func (s *runtimeServer) StartContainer(_ context.Context, r *runtimeapi.StartContainerRequest) (*runtimeapi.StartContainerResponse, error) {
	_, e := s.host.call(opStartContainer, wireObject{ID: r.GetContainerId()})
	return &runtimeapi.StartContainerResponse{}, e
}
func (s *runtimeServer) StopContainer(_ context.Context, r *runtimeapi.StopContainerRequest) (*runtimeapi.StopContainerResponse, error) {
	_, e := s.host.call(opStopContainer, wireObject{ID: r.GetContainerId()})
	return &runtimeapi.StopContainerResponse{}, e
}
func (s *runtimeServer) RemoveContainer(_ context.Context, r *runtimeapi.RemoveContainerRequest) (*runtimeapi.RemoveContainerResponse, error) {
	_, e := s.host.call(opRemoveContainer, wireObject{ID: r.GetContainerId()})
	return &runtimeapi.RemoveContainerResponse{}, e
}
func (s *runtimeServer) ReopenContainerLog(_ context.Context, r *runtimeapi.ReopenContainerLogRequest) (*runtimeapi.ReopenContainerLogResponse, error) {
	_, e := s.host.call(opReopenLog, wireObject{ID: r.GetContainerId()})
	return &runtimeapi.ReopenContainerLogResponse{}, e
}
func containerStatus(o wireObject) *runtimeapi.ContainerStatus {
	return &runtimeapi.ContainerStatus{Id: o.ID, Metadata: &runtimeapi.ContainerMetadata{Name: o.Name, Attempt: o.Attempt}, State: runtimeapi.ContainerState(o.State), CreatedAt: int64(o.CreatedAt), StartedAt: int64(o.StartedAt), FinishedAt: int64(o.FinishedAt), ExitCode: int32(o.ExitCode), Image: &runtimeapi.ImageSpec{Image: o.Image}, ImageRef: o.ImageRef, ImageId: o.ImageRef, Labels: o.Labels, Annotations: o.Annotations, LogPath: o.LogPath, Reason: o.Reason}
}
func (s *runtimeServer) ContainerStatus(_ context.Context, r *runtimeapi.ContainerStatusRequest) (*runtimeapi.ContainerStatusResponse, error) {
	o, e := s.host.call(opContainerStatus, wireObject{ID: r.GetContainerId()})
	if e != nil {
		return nil, e
	}
	return &runtimeapi.ContainerStatusResponse{Status: containerStatus(o)}, nil
}
func (s *runtimeServer) ListContainers(_ context.Context, r *runtimeapi.ListContainersRequest) (*runtimeapi.ListContainersResponse, error) {
	os, e := s.host.list(opListContainers)
	if e != nil {
		return nil, e
	}
	f := r.GetFilter()
	out := []*runtimeapi.Container{}
	for _, o := range os {
		if f != nil && (f.GetId() != "" && f.GetId() != o.ID || f.GetPodSandboxId() != "" && f.GetPodSandboxId() != o.SandboxID || f.GetState() != nil && uint32(f.GetState().GetState()) != o.State || !labelsMatch(o.Labels, f.GetLabelSelector())) {
			continue
		}
		out = append(out, &runtimeapi.Container{Id: o.ID, PodSandboxId: o.SandboxID, Metadata: &runtimeapi.ContainerMetadata{Name: o.Name, Attempt: o.Attempt}, State: runtimeapi.ContainerState(o.State), CreatedAt: int64(o.CreatedAt), Image: &runtimeapi.ImageSpec{Image: o.Image}, ImageRef: o.ImageRef, Labels: o.Labels, Annotations: o.Annotations})
	}
	return &runtimeapi.ListContainersResponse{Containers: out}, nil
}

func (s *imageServer) PullImage(_ context.Context, r *runtimeapi.PullImageRequest) (*runtimeapi.PullImageResponse, error) {
	o, e := s.host.call(opPullImage, wireObject{Image: r.GetImage().GetImage()})
	if e != nil {
		return nil, e
	}
	return &runtimeapi.PullImageResponse{ImageRef: o.ImageRef}, nil
}
func imageFrom(o wireObject) *runtimeapi.Image {
	return &runtimeapi.Image{Id: o.ID, RepoTags: []string{o.Image}, Size: 1, Spec: &runtimeapi.ImageSpec{Image: o.Image}}
}
func (s *imageServer) ImageStatus(_ context.Context, r *runtimeapi.ImageStatusRequest) (*runtimeapi.ImageStatusResponse, error) {
	name := r.GetImage().GetImage()
	o, e := s.host.call(opImageStatus, wireObject{ID: name, Image: name})
	if status.Code(e) == codes.NotFound {
		return &runtimeapi.ImageStatusResponse{}, nil
	}
	if e != nil {
		return nil, e
	}
	return &runtimeapi.ImageStatusResponse{Image: imageFrom(o)}, nil
}
func (s *imageServer) ListImages(context.Context, *runtimeapi.ListImagesRequest) (*runtimeapi.ListImagesResponse, error) {
	os, e := s.host.list(opListImages)
	if e != nil {
		return nil, e
	}
	out := make([]*runtimeapi.Image, 0, len(os))
	for _, o := range os {
		out = append(out, imageFrom(o))
	}
	return &runtimeapi.ListImagesResponse{Images: out}, nil
}
func (s *imageServer) RemoveImage(_ context.Context, r *runtimeapi.RemoveImageRequest) (*runtimeapi.RemoveImageResponse, error) {
	name := r.GetImage().GetImage()
	_, e := s.host.call(opRemoveImage, wireObject{ID: name, Image: name})
	return &runtimeapi.RemoveImageResponse{}, e
}
func (*imageServer) ImageFsInfo(context.Context, *runtimeapi.ImageFsInfoRequest) (*runtimeapi.ImageFsInfoResponse, error) {
	u := &runtimeapi.FilesystemUsage{Timestamp: time.Now().UnixNano(), FsId: &runtimeapi.FilesystemIdentifier{Mountpoint: "/mnt"}, UsedBytes: &runtimeapi.UInt64Value{}, InodesUsed: &runtimeapi.UInt64Value{}}
	return &runtimeapi.ImageFsInfoResponse{ImageFilesystems: []*runtimeapi.FilesystemUsage{u}}, nil
}

func main() {
	if e := os.Remove(socketPath); e != nil && !os.IsNotExist(e) {
		panic(e)
	}
	l, e := net.Listen("unix", socketPath)
	if e != nil {
		panic(e)
	}
	host := &hostClient{fd: -1}
	server := grpc.NewServer()
	runtimeapi.RegisterRuntimeServiceServer(server, &runtimeServer{host: host})
	runtimeapi.RegisterImageServiceServer(server, &imageServer{host: host})
	fmt.Printf("cri-shim: CRI v1 transport listening on %s; host CID %d port %d\n", socketPath, hostCID, hostPort)
	if e = server.Serve(l); e != nil {
		panic(e)
	}
}
