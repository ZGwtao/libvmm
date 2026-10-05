/* SPDX-License-Identifier: BSD-2-Clause */
#include "runtime_protocol.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct cri_object sandboxes[CRI_MAX_OBJECTS];
static struct cri_object containers[CRI_MAX_OBJECTS];
static struct cri_object images[CRI_MAX_OBJECTS];
static uint64_t next_id;

static uint64_t now_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts)) return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static struct cri_object *find(struct cri_object *table, const char *id)
{
	for (unsigned i = 0; i < CRI_MAX_OBJECTS; i++)
		if (table[i].present && !strcmp(table[i].id, id)) return &table[i];
	return NULL;
}

static struct cri_object *allocate(struct cri_object *table)
{
	for (unsigned i = 0; i < CRI_MAX_OBJECTS; i++) if (!table[i].present) {
		memset(&table[i], 0, sizeof(table[i])); table[i].present = 1; return &table[i];
	}
	return NULL;
}

static uint32_t list_one(struct cri_object *table, uint32_t cursor,
			 struct cri_object *response)
{
	for (unsigned i = cursor; i < CRI_MAX_OBJECTS; i++) if (table[i].present) {
		*response = table[i]; response->cursor = i + 1; return CRI_OK;
	}
	response->present = 0; response->cursor = CRI_MAX_OBJECTS; return CRI_OK;
}

uint32_t cri_dispatch(uint16_t op, const struct cri_object *request,
		      struct cri_object *response)
{
	struct cri_object *o;
	memset(response, 0, sizeof(*response));
	switch (op) {
	case CRI_HELLO: response->present = 1; return CRI_OK;
	case CRI_RUN_SANDBOX:
		o = allocate(sandboxes); if (!o) return CRI_FULL; *o = *request; o->present = 1;
		snprintf(o->id, sizeof(o->id), "dummy-sandbox-%08llx", (unsigned long long)++next_id);
		o->state = 0; o->created_at = now_ns(); strcpy(o->ip, "192.0.2.1"); strcpy(o->reason, "DummyReady");
		*response = *o; printf("HOST_RUNTIME: RunPodSandbox name=%s namespace=%s id=%s\n", o->name, o->namespace_, o->id); return CRI_OK;
	case CRI_STOP_SANDBOX: o = find(sandboxes, request->id); if (o) o->state = 1; return CRI_OK;
	case CRI_REMOVE_SANDBOX: o = find(sandboxes, request->id); if (o) memset(o, 0, sizeof(*o)); return CRI_OK;
	case CRI_SANDBOX_STATUS: o = find(sandboxes, request->id); if (!o) return CRI_NOT_FOUND; *response = *o; return CRI_OK;
	case CRI_LIST_SANDBOX: return list_one(sandboxes, request->cursor, response);
	case CRI_CREATE_CONTAINER:
		o = allocate(containers); if (!o) return CRI_FULL; *o = *request; o->present = 1;
		snprintf(o->id, sizeof(o->id), "dummy-container-%08llx", (unsigned long long)++next_id);
		o->state = 0; o->created_at = now_ns(); snprintf(o->image_ref, sizeof(o->image_ref), "dummy-image://%s", o->image); strcpy(o->reason, "DummyCreated");
		*response = *o; printf("HOST_RUNTIME: CreateContainer name=%s sandbox=%s id=%s\n", o->name, o->sandbox_id, o->id); return CRI_OK;
	case CRI_START_CONTAINER: o = find(containers, request->id); if (!o) return CRI_NOT_FOUND; o->state = 1; o->started_at = now_ns(); strcpy(o->reason, "DummyRunning"); printf("HOST_RUNTIME: StartContainer id=%s\n", o->id); return CRI_OK;
	case CRI_STOP_CONTAINER: o = find(containers, request->id); if (o) { o->state = 2; o->finished_at = now_ns(); strcpy(o->reason, "DummyStopped"); } return CRI_OK;
	case CRI_REMOVE_CONTAINER: o = find(containers, request->id); if (o) memset(o, 0, sizeof(*o)); return CRI_OK;
	case CRI_CONTAINER_STATUS: o = find(containers, request->id); if (!o) return CRI_NOT_FOUND; *response = *o; return CRI_OK;
	case CRI_LIST_CONTAINER: return list_one(containers, request->cursor, response);
	case CRI_REOPEN_LOG: return find(containers, request->id) ? CRI_OK : CRI_NOT_FOUND;
	case CRI_PULL_IMAGE:
		for (unsigned i=0;i<CRI_MAX_OBJECTS;i++) if (images[i].present && !strcmp(images[i].image, request->image)) { *response=images[i]; return CRI_OK; }
		o=allocate(images); if(!o) return CRI_FULL; *o=*request; o->present=1; snprintf(o->image_ref,sizeof(o->image_ref),"dummy-image://%s",o->image); strncpy(o->id,o->image_ref,sizeof(o->id)-1); o->id[sizeof(o->id)-1]=0; *response=*o; printf("HOST_RUNTIME: PullImage image=%s ref=%s\n",o->image,o->image_ref); return CRI_OK;
	case CRI_REMOVE_IMAGE: o=find(images,request->id); if(!o) for(unsigned i=0;i<CRI_MAX_OBJECTS;i++) if(images[i].present&&!strcmp(images[i].image,request->image)){o=&images[i];break;} if(o) memset(o,0,sizeof(*o)); return CRI_OK;
	case CRI_IMAGE_STATUS: o=find(images,request->id); if(!o) for(unsigned i=0;i<CRI_MAX_OBJECTS;i++) if(images[i].present&&!strcmp(images[i].image,request->image)){o=&images[i];break;} if(!o)return CRI_NOT_FOUND; *response=*o; return CRI_OK;
	case CRI_LIST_IMAGE: return list_one(images, request->cursor, response);
	default: return CRI_INVALID;
	}
}
