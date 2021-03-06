/* Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/skbuff.h>
#include <linux/debugobjects.h>

#ifndef _LINUX_SKBBUFF_DEBUG_OBJECTS
#define _LINUX_SKBBUFF_DEBUG_OBJECTS

#ifdef CONFIG_DEBUG_OBJECTS_SKBUFF
void skbuff_debugobj_init_and_activate(struct sk_buff *skb);
void skbuff_debugobj_activate(struct sk_buff *skb);
void skbuff_debugobj_deactivate(struct sk_buff *skb);
void skbuff_debugobj_destroy(struct sk_buff *skb);
void skbuff_debugobj_sum_validate(struct sk_buff *skb);
void skbuff_debugobj_sum_update(struct sk_buff *skb);
#else
static inline void skbuff_debugobj_init_and_activate(struct sk_buff *skb) { }
static inline void skbuff_debugobj_activate(struct sk_buff *skb) { }
static inline void skbuff_debugobj_deactivate(struct sk_buff *skb) { }
static inline void skbuff_debugobj_destroy(struct sk_buff *skb) { }
static inline void skbuff_debugobj_sum_validate(struct sk_buff *skb) { }
static inline void skbuff_debugobj_sum_update(struct sk_buff *skb) { }
#endif

#endif /* _LINUX_SKBBUFF_DEBUG_OBJECTS */
