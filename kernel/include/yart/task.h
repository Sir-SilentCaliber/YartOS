#pragma once
#include <yart/fs.h>
vnode_t *task_cwd(void);
void     task_set_cwd(vnode_t *v);
void     task_init(void);
