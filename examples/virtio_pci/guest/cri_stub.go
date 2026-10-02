// Copyright 2026
// SPDX-License-Identifier: BSD-2-Clause

// cri-stub provides an in-memory CRI v1 lifecycle implementation for a kubelet
// proof of concept. Operations succeed with dummy objects; no workload is run.
package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	runtimeapi "k8s.io/cri-api/pkg/apis/runtime/v1"
)

const socketPath = "/run/cri-vsock.sock"

type runtimeServer struct {
	runtimeapi.UnimplementedRuntimeServiceServer
	mu                 sync.RWMutex
	sandboxes          map[string]*runtimeapi.PodSandboxStatus
	sandboxLogDirs     map[string]string
	containers         map[string]*runtimeapi.ContainerStatus
	containerSandboxes map[string]string
	nextID             atomic.Uint64
}

func newRuntimeServer() *runtimeServer {
	return &runtimeServer{
		sandboxes:          make(map[string]*runtimeapi.PodSandboxStatus),
		sandboxLogDirs:     make(map[string]string),
		containers:         make(map[string]*runtimeapi.ContainerStatus),
		containerSandboxes: make(map[string]string),
	}
}
func (s *runtimeServer) id(kind string) string {
	return fmt.Sprintf("dummy-%s-%08x", kind, s.nextID.Add(1))
}

func (*runtimeServer) Version(_ context.Context, req *runtimeapi.VersionRequest) (*runtimeapi.VersionResponse, error) {
	return &runtimeapi.VersionResponse{
		Version:           req.Version,
		RuntimeName:       "libvmm-cri-stub",
		RuntimeVersion:    "0.1.0",
		RuntimeApiVersion: "v1",
	}, nil
}

func (*runtimeServer) Status(_ context.Context, _ *runtimeapi.StatusRequest) (*runtimeapi.StatusResponse, error) {
	return &runtimeapi.StatusResponse{Status: &runtimeapi.RuntimeStatus{Conditions: []*runtimeapi.RuntimeCondition{
		{Type: "RuntimeReady", Status: true, Reason: "DummyBackend", Message: "in-memory CRI lifecycle shim is ready"},
		{Type: "NetworkReady", Status: true, Reason: "DummyBackend", Message: "dummy pod networking is ready"},
	}}}, nil
}

func (*runtimeServer) RuntimeConfig(_ context.Context, _ *runtimeapi.RuntimeConfigRequest) (*runtimeapi.RuntimeConfigResponse, error) {
	return &runtimeapi.RuntimeConfigResponse{Linux: &runtimeapi.LinuxRuntimeConfiguration{
		CgroupDriver: runtimeapi.CgroupDriver_CGROUPFS,
	}}, nil
}

func (*runtimeServer) UpdateRuntimeConfig(_ context.Context, _ *runtimeapi.UpdateRuntimeConfigRequest) (*runtimeapi.UpdateRuntimeConfigResponse, error) {
	// Keep this a no-op until the native runtime bridge consumes PodCIDR data.
	return &runtimeapi.UpdateRuntimeConfigResponse{}, nil
}

func (s *runtimeServer) RunPodSandbox(_ context.Context, req *runtimeapi.RunPodSandboxRequest) (*runtimeapi.RunPodSandboxResponse, error) {
	id, config := s.id("sandbox"), req.GetConfig()
	namespaceOptions := config.GetLinux().GetSecurityContext().GetNamespaceOptions()
	entry := &runtimeapi.PodSandboxStatus{
		Id: id, Metadata: config.GetMetadata(), State: runtimeapi.PodSandboxState_SANDBOX_READY,
		CreatedAt: time.Now().UnixNano(), Network: &runtimeapi.PodSandboxNetworkStatus{Ip: "192.0.2.1"},
		Linux:  &runtimeapi.LinuxPodSandboxStatus{Namespaces: &runtimeapi.Namespace{Options: namespaceOptions}},
		Labels: config.GetLabels(), Annotations: config.GetAnnotations(), RuntimeHandler: req.GetRuntimeHandler(),
	}
	s.mu.Lock()
	s.sandboxes[id] = entry
	s.sandboxLogDirs[id] = config.GetLogDirectory()
	s.mu.Unlock()
	fmt.Printf("cri-shim: RunPodSandbox name=%s namespace=%s id=%s\n", entry.Metadata.GetName(), entry.Metadata.GetNamespace(), id)
	return &runtimeapi.RunPodSandboxResponse{PodSandboxId: id}, nil
}

func (s *runtimeServer) StopPodSandbox(_ context.Context, req *runtimeapi.StopPodSandboxRequest) (*runtimeapi.StopPodSandboxResponse, error) {
	s.mu.Lock()
	if e := s.sandboxes[req.GetPodSandboxId()]; e != nil {
		e.State = runtimeapi.PodSandboxState_SANDBOX_NOTREADY
	}
	s.mu.Unlock()
	return &runtimeapi.StopPodSandboxResponse{}, nil
}
func (s *runtimeServer) RemovePodSandbox(_ context.Context, req *runtimeapi.RemovePodSandboxRequest) (*runtimeapi.RemovePodSandboxResponse, error) {
	s.mu.Lock()
	delete(s.sandboxes, req.GetPodSandboxId())
	delete(s.sandboxLogDirs, req.GetPodSandboxId())
	s.mu.Unlock()
	return &runtimeapi.RemovePodSandboxResponse{}, nil
}
func (s *runtimeServer) PodSandboxStatus(_ context.Context, req *runtimeapi.PodSandboxStatusRequest) (*runtimeapi.PodSandboxStatusResponse, error) {
	s.mu.RLock()
	e := s.sandboxes[req.GetPodSandboxId()]
	s.mu.RUnlock()
	if e == nil {
		return nil, status.Error(codes.NotFound, "dummy sandbox not found")
	}
	return &runtimeapi.PodSandboxStatusResponse{Status: e}, nil
}
func labelsMatch(labels, selector map[string]string) bool {
	for key, value := range selector {
		if labels[key] != value {
			return false
		}
	}
	return true
}

func (s *runtimeServer) ListPodSandbox(_ context.Context, req *runtimeapi.ListPodSandboxRequest) (*runtimeapi.ListPodSandboxResponse, error) {
	filter := req.GetFilter()
	s.mu.RLock()
	items := make([]*runtimeapi.PodSandbox, 0, len(s.sandboxes))
	for _, e := range s.sandboxes {
		if filter != nil && (filter.GetId() != "" && filter.GetId() != e.Id ||
			filter.GetState() != nil && filter.GetState().GetState() != e.State ||
			!labelsMatch(e.Labels, filter.GetLabelSelector())) {
			continue
		}
		items = append(items, &runtimeapi.PodSandbox{Id: e.Id, Metadata: e.Metadata, State: e.State, CreatedAt: e.CreatedAt, Labels: e.Labels, Annotations: e.Annotations, RuntimeHandler: e.RuntimeHandler})
	}
	s.mu.RUnlock()
	return &runtimeapi.ListPodSandboxResponse{Items: items}, nil
}
func (s *runtimeServer) CreateContainer(_ context.Context, req *runtimeapi.CreateContainerRequest) (*runtimeapi.CreateContainerResponse, error) {
	id, config := s.id("container"), req.GetConfig()
	imageRef := "dummy-image://" + config.GetImage().GetImage()
	s.mu.RLock()
	logPath := filepath.Join(s.sandboxLogDirs[req.GetPodSandboxId()], config.GetLogPath())
	s.mu.RUnlock()
	if err := os.MkdirAll(filepath.Dir(logPath), 0755); err != nil {
		return nil, status.Errorf(codes.Internal, "create dummy log directory: %v", err)
	}
	if f, err := os.OpenFile(logPath, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644); err != nil {
		return nil, status.Errorf(codes.Internal, "create dummy container log: %v", err)
	} else {
		_ = f.Close()
	}
	e := &runtimeapi.ContainerStatus{Id: id, Metadata: config.GetMetadata(), State: runtimeapi.ContainerState_CONTAINER_CREATED, CreatedAt: time.Now().UnixNano(), Image: config.GetImage(), ImageRef: imageRef, ImageId: imageRef, Labels: config.GetLabels(), Annotations: config.GetAnnotations(), Mounts: config.GetMounts(), LogPath: logPath, Reason: "DummyCreated"}
	s.mu.Lock()
	s.containers[id] = e
	s.containerSandboxes[id] = req.GetPodSandboxId()
	s.mu.Unlock()
	fmt.Printf("cri-shim: CreateContainer name=%s sandbox=%s id=%s\n", e.Metadata.GetName(), req.GetPodSandboxId(), id)
	return &runtimeapi.CreateContainerResponse{ContainerId: id}, nil
}
func (s *runtimeServer) StartContainer(_ context.Context, req *runtimeapi.StartContainerRequest) (*runtimeapi.StartContainerResponse, error) {
	s.mu.Lock()
	e := s.containers[req.GetContainerId()]
	if e != nil {
		e.State, e.StartedAt, e.Reason = runtimeapi.ContainerState_CONTAINER_RUNNING, time.Now().UnixNano(), "DummyRunning"
	}
	s.mu.Unlock()
	if e == nil {
		return nil, status.Error(codes.NotFound, "dummy container not found")
	}
	fmt.Printf("cri-shim: StartContainer id=%s\n", req.GetContainerId())
	return &runtimeapi.StartContainerResponse{}, nil
}
func (s *runtimeServer) StopContainer(_ context.Context, req *runtimeapi.StopContainerRequest) (*runtimeapi.StopContainerResponse, error) {
	s.mu.Lock()
	if e := s.containers[req.GetContainerId()]; e != nil {
		e.State, e.FinishedAt, e.Reason = runtimeapi.ContainerState_CONTAINER_EXITED, time.Now().UnixNano(), "DummyStopped"
	}
	s.mu.Unlock()
	return &runtimeapi.StopContainerResponse{}, nil
}
func (s *runtimeServer) RemoveContainer(_ context.Context, req *runtimeapi.RemoveContainerRequest) (*runtimeapi.RemoveContainerResponse, error) {
	s.mu.Lock()
	delete(s.containers, req.GetContainerId())
	delete(s.containerSandboxes, req.GetContainerId())
	s.mu.Unlock()
	return &runtimeapi.RemoveContainerResponse{}, nil
}
func (s *runtimeServer) ReopenContainerLog(_ context.Context, req *runtimeapi.ReopenContainerLogRequest) (*runtimeapi.ReopenContainerLogResponse, error) {
	s.mu.RLock()
	e := s.containers[req.GetContainerId()]
	s.mu.RUnlock()
	if e == nil {
		return nil, status.Error(codes.NotFound, "dummy container not found")
	}
	return &runtimeapi.ReopenContainerLogResponse{}, nil
}
func (s *runtimeServer) ContainerStatus(_ context.Context, req *runtimeapi.ContainerStatusRequest) (*runtimeapi.ContainerStatusResponse, error) {
	s.mu.RLock()
	e := s.containers[req.GetContainerId()]
	s.mu.RUnlock()
	if e == nil {
		return nil, status.Error(codes.NotFound, "dummy container not found")
	}
	return &runtimeapi.ContainerStatusResponse{Status: e}, nil
}
func (s *runtimeServer) ListContainers(_ context.Context, req *runtimeapi.ListContainersRequest) (*runtimeapi.ListContainersResponse, error) {
	filter := req.GetFilter()
	s.mu.RLock()
	items := make([]*runtimeapi.Container, 0, len(s.containers))
	for _, e := range s.containers {
		sandboxID := s.containerSandboxes[e.Id]
		if filter != nil && (filter.GetId() != "" && filter.GetId() != e.Id ||
			filter.GetPodSandboxId() != "" && filter.GetPodSandboxId() != sandboxID ||
			filter.GetState() != nil && filter.GetState().GetState() != e.State ||
			!labelsMatch(e.Labels, filter.GetLabelSelector())) {
			continue
		}
		items = append(items, &runtimeapi.Container{Id: e.Id, PodSandboxId: sandboxID, Metadata: e.Metadata, State: e.State, CreatedAt: e.CreatedAt, Image: e.Image, ImageRef: e.ImageRef, Labels: e.Labels, Annotations: e.Annotations})
	}
	s.mu.RUnlock()
	return &runtimeapi.ListContainersResponse{Containers: items}, nil
}

type imageServer struct {
	runtimeapi.UnimplementedImageServiceServer
	mu     sync.RWMutex
	images map[string]*runtimeapi.Image
}

func newImageServer() *imageServer { return &imageServer{images: make(map[string]*runtimeapi.Image)} }

func (s *imageServer) ListImages(_ context.Context, _ *runtimeapi.ListImagesRequest) (*runtimeapi.ListImagesResponse, error) {
	s.mu.RLock()
	items := make([]*runtimeapi.Image, 0, len(s.images))
	for _, e := range s.images {
		items = append(items, e)
	}
	s.mu.RUnlock()
	return &runtimeapi.ListImagesResponse{Images: items}, nil
}
func (s *imageServer) PullImage(_ context.Context, req *runtimeapi.PullImageRequest) (*runtimeapi.PullImageResponse, error) {
	name := req.GetImage().GetImage()
	ref := "dummy-image://" + name
	s.mu.Lock()
	s.images[ref] = &runtimeapi.Image{Id: ref, RepoTags: []string{name}, Size: 1, Spec: req.GetImage()}
	s.mu.Unlock()
	fmt.Printf("cri-shim: PullImage image=%s ref=%s\n", name, ref)
	return &runtimeapi.PullImageResponse{ImageRef: ref}, nil
}
func (s *imageServer) ImageStatus(_ context.Context, req *runtimeapi.ImageStatusRequest) (*runtimeapi.ImageStatusResponse, error) {
	ref := "dummy-image://" + req.GetImage().GetImage()
	s.mu.RLock()
	e := s.images[ref]
	s.mu.RUnlock()
	return &runtimeapi.ImageStatusResponse{Image: e}, nil
}
func (s *imageServer) RemoveImage(_ context.Context, req *runtimeapi.RemoveImageRequest) (*runtimeapi.RemoveImageResponse, error) {
	name := req.GetImage().GetImage()
	s.mu.Lock()
	delete(s.images, name)
	delete(s.images, "dummy-image://"+name)
	s.mu.Unlock()
	return &runtimeapi.RemoveImageResponse{}, nil
}

func (*imageServer) ImageFsInfo(_ context.Context, _ *runtimeapi.ImageFsInfoRequest) (*runtimeapi.ImageFsInfoResponse, error) {
	usage := &runtimeapi.FilesystemUsage{
		Timestamp:  time.Now().UnixNano(),
		FsId:       &runtimeapi.FilesystemIdentifier{Mountpoint: "/mnt"},
		UsedBytes:  &runtimeapi.UInt64Value{Value: 0},
		InodesUsed: &runtimeapi.UInt64Value{Value: 0},
	}
	return &runtimeapi.ImageFsInfoResponse{ImageFilesystems: []*runtimeapi.FilesystemUsage{usage}}, nil
}

func main() {
	if err := os.Remove(socketPath); err != nil && !os.IsNotExist(err) {
		panic(err)
	}

	listener, err := net.Listen("unix", socketPath)
	if err != nil {
		panic(err)
	}

	server := grpc.NewServer()
	runtimeapi.RegisterRuntimeServiceServer(server, newRuntimeServer())
	runtimeapi.RegisterImageServiceServer(server, newImageServer())
	fmt.Printf("cri-shim: dummy CRI v1 backend listening on %s\n", socketPath)
	if err := server.Serve(listener); err != nil {
		panic(err)
	}
}
