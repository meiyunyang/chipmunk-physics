/* Copyright (c) 2009 Scott Lembcke
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "stdlib.h"
#include "stdio.h"

#include "chipmunk_private.h"

static inline cpSpatialIndexClass *Klass();

typedef struct Node Node;
typedef struct Pair Pair;

struct cpBBTree {
	cpSpatialIndex spatialIndex;
	cpBBTreeVelocityFunc velocityFunc;
	
	cpHashSet *leaves;
	Node *root;
	
	Node *pooledNodes;
	Pair *pooledPairs;
	cpArray *allocatedBuffers;
	
	cpTimestamp stamp;
};

struct Node {
	void *obj;
	cpBB bb;
	Node *parent;
	
	union {
		// Internal nodes
		struct { Node *a, *b; };
		
		// Leaves
		struct {
			cpTimestamp stamp;
			Pair *pairs;
		};
	};
};

typedef struct Thread {
	Pair *prev;
	Node *leaf;
	Pair *next;
} Thread;

struct Pair { Thread a, b; };

#pragma mark Misc Functions

//static inline cpFloat
//cpBBProximity(cpBB a, cpBB b)
//{
//	return cpfabs(a.l + a.r - b.l - b.r) + cpfabs(a.b + b.t - b.b - b.t);
//}

static inline cpBB
GetBB(cpBBTree *tree, void *obj)
{
	cpBB bb = tree->spatialIndex.bbfunc(obj);
	
	cpBBTreeVelocityFunc velocityFunc = tree->velocityFunc;
	if(velocityFunc){
		cpFloat coef = 0.1f;
		cpFloat x = (bb.r - bb.l)*coef;
		cpFloat y = (bb.t - bb.b)*coef;
		
		cpVect v = cpvmult(velocityFunc(obj), 0.1f);
		return cpBBNew(bb.l + cpfmin(-x, v.x), bb.b + cpfmin(-y, v.y), bb.r + cpfmax(x, v.x), bb.t + cpfmax(y, v.y));
	} else {
		return bb;
	}
}

static inline cpBBTree *
GetTree(cpSpatialIndex *index)
{
	return (index && index->klass == Klass() ? (cpBBTree *)index : NULL);
}

static inline Node *
GetRootIfTree(cpSpatialIndex *index){
	return (index && index->klass == Klass() ? ((cpBBTree *)index)->root : NULL);
}

static inline cpTimestamp
GetStamp(cpBBTree *tree)
{
	cpBBTree *dynamicTree = GetTree(tree->spatialIndex.dynamicIndex);
	return (dynamicTree ? dynamicTree->stamp : tree->stamp);
}

static inline void
IncrementStamp(cpBBTree *tree)
{
	cpBBTree *dynamicTree = GetTree(tree->spatialIndex.dynamicIndex);
	if(dynamicTree){
		dynamicTree->stamp++;
	} else {
		tree->stamp++;
	}
}

#pragma mark Pair/Thread Functions

static void
PairRecycle(cpBBTree *tree, Pair *pair)
{
	pair->a.next = tree->pooledPairs;
	tree->pooledPairs = pair;
}

static Pair *
PairFromPool(cpBBTree *tree)
{
	Pair *pair = tree->pooledPairs;
	
	if(pair){
		tree->pooledPairs = pair->a.next;
		return pair;
	} else {
		// Pool is exhausted, make more
		int count = CP_BUFFER_BYTES/sizeof(Pair);
		cpAssertSoft(count, "Buffer size is too small.");
		
		Pair *buffer = (Pair *)cpcalloc(1, CP_BUFFER_BYTES);
		cpArrayPush(tree->allocatedBuffers, buffer);
		
		// push all but the first one, return the first instead
		for(int i=1; i<count; i++) PairRecycle(tree, buffer + i);
		return buffer;
	}
}

static inline void
ThreadUnlink(Thread thread)
{
	Pair *next = thread.next;
	Pair *prev = thread.prev;
	
	if(next){
		if(next->a.leaf == thread.leaf) next->a.prev = prev; else next->b.prev = prev;
	}
	
	if(prev){
		if(prev->a.leaf == thread.leaf) prev->a.next = next; else prev->b.next = next;
	} else {
		thread.leaf->pairs = next;
	}
}

static void
PairsClear(Node *leaf, cpBBTree *tree)
{
	Pair *pair = leaf->pairs;
	leaf->pairs = NULL;
	
	while(pair){
		if(pair->a.leaf == leaf){
			Pair *next = pair->a.next;
			ThreadUnlink(pair->b);
			PairRecycle(tree, pair);
			pair = next;
		} else {
			Pair *next = pair->b.next;
			ThreadUnlink(pair->a);
			PairRecycle(tree, pair);
			pair = next;
		}
	}
}

static void
PairInsert(Node *a, Node *b, cpBBTree *tree)
{
	Pair *nextA = a->pairs, *nextB = b->pairs;
	Pair *pair = PairFromPool(tree);
	Pair temp = {{NULL, a, nextA},{NULL, b, nextB}};
	
	a->pairs = b->pairs = pair;
	*pair = temp;
	
	if(nextA){
		if(nextA->a.leaf == a) nextA->a.prev = pair; else nextA->b.prev = pair;
	}
	
	if(nextB){
		if(nextB->a.leaf == b) nextB->a.prev = pair; else nextB->b.prev = pair;
	}
}


#pragma mark Node Functions

static void
NodeRecycle(cpBBTree *tree, Node *node)
{
	node->parent = tree->pooledNodes;
	tree->pooledNodes = node;
}

static Node *
NodeFromPool(cpBBTree *tree)
{
	Node *node = tree->pooledNodes;
	
	if(node){
		tree->pooledNodes = node->parent;
		return node;
	} else {
		// Pool is exhausted, make more
		int count = CP_BUFFER_BYTES/sizeof(Node);
		cpAssertSoft(count, "Buffer size is too small.");
		
		Node *buffer = (Node *)cpcalloc(1, CP_BUFFER_BYTES);
		cpArrayPush(tree->allocatedBuffers, buffer);
		
		// push all but the first one, return the first instead
		for(int i=1; i<count; i++) NodeRecycle(tree, buffer + i);
		return buffer;
	}
}

static inline void
NodeSetA(Node *node, Node *value)
{
	node->a = value;
	value->parent = node;
}

static inline void
NodeSetB(Node *node, Node *value)
{
	node->b = value;
	value->parent = node;
}

static Node *
NodeNew(cpBBTree *tree, Node *a, Node *b)
{
	Node *node = NodeFromPool(tree);
	
	node->obj = NULL;
	node->bb = cpBBMerge(a->bb, b->bb);
	node->parent = NULL;
	
	NodeSetA(node, a);
	NodeSetB(node, b);
	
	return node;
}

static inline cpBool
NodeIsLeaf(Node *node)
{
	return (node->obj != NULL);
}

static inline Node *
NodeOther(Node *node, Node *child)
{
	return (node->a == child ? node->b : node->a);
}

static inline void
NodeReplaceChild(Node *parent, Node *child, Node *value, cpBBTree *tree)
{
	cpAssertSoft(!NodeIsLeaf(parent), "Cannot replace child of a leaf.");
	cpAssertSoft(child == parent->a || child == parent->b, "Node is not a child of parent.");
	
	if(parent->a == child){
		NodeRecycle(tree, parent->a);
		NodeSetA(parent, value);
	} else {
		NodeRecycle(tree, parent->b);
		NodeSetB(parent, value);
	}
	
	for(Node *node=parent; node; node = node->parent){
		node->bb = cpBBMerge(node->a->bb, node->b->bb);
	}
}

#pragma mark Subtree Functions

static Node *
SubtreeInsert(Node *subtree, Node *leaf, cpBBTree *tree)
{
	if(subtree == NULL){
		return leaf;
	} else if(NodeIsLeaf(subtree)){
		return NodeNew(tree, leaf, subtree);
	} else {
		cpFloat cost_a = cpBBArea(subtree->b->bb) + cpBBMergedArea(subtree->a->bb, leaf->bb);
		cpFloat cost_b = cpBBArea(subtree->a->bb) + cpBBMergedArea(subtree->b->bb, leaf->bb);
		
//		cpFloat cost_a = cpBBProximity(subtree->a->bb, leaf->bb);
//		cpFloat cost_b = cpBBProximity(subtree->b->bb, leaf->bb);
		
		if(cost_b < cost_a){
			NodeSetB(subtree, SubtreeInsert(subtree->b, leaf, tree));
		} else {
			NodeSetA(subtree, SubtreeInsert(subtree->a, leaf, tree));
		}
		
		subtree->bb = cpBBMerge(subtree->bb, leaf->bb);
		return subtree;
	}
}

static void
SubtreeQuery(Node *subtree, void *obj, cpBB bb, cpSpatialIndexQueryFunc func, void *data)
{
	if(cpBBIntersects(subtree->bb, bb)){
		if(NodeIsLeaf(subtree)){
			func(obj, subtree->obj, data);
		} else {
			SubtreeQuery(subtree->a, obj, bb, func, data);
			SubtreeQuery(subtree->b, obj, bb, func, data);
		}
	}
}


// TODO Needs early exit optimization for ray queries
static void
SubtreeSegmentQuery(Node *subtree, void *obj, cpVect a, cpVect b, cpSpatialIndexSegmentQueryFunc func, void *data)
{
	if(cpBBIntersectsSegment(subtree->bb, a, b)){
		if(NodeIsLeaf(subtree)){
			func(obj, subtree->obj, data);
		} else {
			SubtreeSegmentQuery(subtree->a, obj, a, b, func, data);
			SubtreeSegmentQuery(subtree->b, obj, a, b, func, data);
		}
	}
}

static void
SubtreeRecycle(cpBBTree *tree, Node *node)
{
	if(!NodeIsLeaf(node)){
		SubtreeRecycle(tree, node->a);
		SubtreeRecycle(tree, node->b);
		NodeRecycle(tree, node);
	}
}

static inline Node *
SubtreeRemove(Node *subtree, Node *leaf, cpBBTree *tree)
{
	if(leaf == subtree){
		return NULL;
	} else {
		Node *parent = leaf->parent;
		if(parent == subtree){
			Node *other = NodeOther(subtree, leaf);
			other->parent = subtree->parent;
			NodeRecycle(tree, subtree);
			return other;
		} else {
			NodeReplaceChild(parent->parent, parent, NodeOther(parent, leaf), tree);
			return subtree;
		}
	}
}

#pragma mark Marking Functions

typedef struct MarkContext {
	cpBBTree *tree;
	Node *staticRoot;
	cpSpatialIndexQueryFunc func;
	void *data;
} MarkContext;

static void
MarkLeafQuery(Node *subtree, Node *leaf, cpBool left, MarkContext *context)
{
	if(cpBBIntersects(leaf->bb, subtree->bb)){
		if(NodeIsLeaf(subtree)){
			if(left){
				PairInsert(leaf, subtree, context->tree);
			} else {
				if(subtree->stamp < leaf->stamp) PairInsert(subtree, leaf, context->tree);
				context->func(leaf->obj, subtree->obj, context->data);
			}
		} else {
			MarkLeafQuery(subtree->a, leaf, left, context);
			MarkLeafQuery(subtree->b, leaf, left, context);
		}
	}
}

static void
MarkLeaf(Node *leaf, MarkContext *context)
{
	cpBBTree *tree = context->tree;
	if(leaf->stamp == GetStamp(tree)){
		Node *staticRoot = context->staticRoot;
		if(staticRoot) MarkLeafQuery(staticRoot, leaf, cpFalse, context);
		
		for(Node *node = leaf; node->parent; node = node->parent){
			if(node == node->parent->a){
				MarkLeafQuery(node->parent->b, leaf, cpTrue, context);
			} else {
				MarkLeafQuery(node->parent->a, leaf, cpFalse, context);
			}
		}
	} else {
		Pair *pair = leaf->pairs;
		while(pair){
			if(leaf == pair->b.leaf){
				context->func(pair->a.leaf->obj, leaf->obj, context->data);
				pair = pair->b.next;
			} else {
				pair = pair->a.next;
			}
		}
	}
}

static void
MarkSubtree(Node *subtree, MarkContext *context)
{
	if(NodeIsLeaf(subtree)){
		MarkLeaf(subtree, context);
	} else {
		MarkSubtree(subtree->a, context);
		MarkSubtree(subtree->b, context);
	}
}

#pragma mark Leaf Functions

static Node *
LeafNew(cpBBTree *tree, void *obj, cpBB bb)
{
	Node *node = NodeFromPool(tree);
	node->obj = obj;
	node->bb = GetBB(tree, obj);
	
	node->parent = NULL;
	node->stamp = 0;
	node->pairs = NULL;
	
	return node;
}

static cpBool
LeafUpdate(Node *leaf, cpBBTree *tree)
{
	Node *root = tree->root;
	cpBB bb = tree->spatialIndex.bbfunc(leaf->obj);
	
	if(!cpBBContainsBB(leaf->bb, bb)){
		leaf->bb = GetBB(tree, leaf->obj);
		
		root = SubtreeRemove(root, leaf, tree);
		tree->root = SubtreeInsert(root, leaf, tree);
		
		PairsClear(leaf, tree);
		leaf->stamp = GetStamp(tree);
		
		return cpTrue;
	}
	
	return cpFalse;
}

static void VoidQueryFunc(void *obj1, void *obj2, void *data){}

static void
LeafAddPairs(Node *leaf, cpBBTree *tree)
{
	cpSpatialIndex *dynamicIndex = tree->spatialIndex.dynamicIndex;
	if(dynamicIndex){
		Node *dynamicRoot = GetRootIfTree(dynamicIndex);
		if(dynamicRoot){
			cpBBTree *dynamicTree = GetTree(dynamicIndex);
			MarkContext context = {dynamicTree, NULL, NULL, NULL};
			MarkLeafQuery(dynamicRoot, leaf, cpTrue, &context);
		}
	} else {
		Node *staticRoot = GetRootIfTree(tree->spatialIndex.staticIndex);
		MarkContext context = {tree, staticRoot, VoidQueryFunc, NULL};
		MarkLeaf(leaf, &context);
	}
}

#pragma mark Memory Management Functions

cpBBTree *
cpBBTreeAlloc(void)
{
	return (cpBBTree *)cpcalloc(1, sizeof(cpBBTree));
}

static int
leafSetEql(void *obj, Node *node)
{
	return (obj == node->obj);
}

static void *
leafSetTrans(void *obj, cpBBTree *tree)
{
	return LeafNew(tree, obj, tree->spatialIndex.bbfunc(obj));
}

cpSpatialIndex *
cpBBTreeInit(cpBBTree *tree, cpSpatialIndexBBFunc bbfunc, cpSpatialIndex *staticIndex)
{
	cpSpatialIndexInit((cpSpatialIndex *)tree, Klass(), bbfunc, staticIndex);
	
	tree->velocityFunc = NULL;
	
	tree->leaves = cpHashSetNew(0, (cpHashSetEqlFunc)leafSetEql);
	tree->root = NULL;
	
	tree->pooledNodes = NULL;
	tree->allocatedBuffers = cpArrayNew(0);
	
	tree->stamp = 0;
	
	return (cpSpatialIndex *)tree;
}

void
cpBBTreeSetVelocityFunc(cpSpatialIndex *index, cpBBTreeVelocityFunc func)
{
	if(index->klass != Klass()){
		cpAssertWarn(cpFalse, "Ignoring cpBBTreeSetVelocityFunc() call to non-tree spatial index.");
		return;
	}
	
	((cpBBTree *)index)->velocityFunc = func;
}

cpSpatialIndex *
cpBBTreeNew(cpSpatialIndexBBFunc bbfunc, cpSpatialIndex *staticIndex)
{
	return cpBBTreeInit(cpBBTreeAlloc(), bbfunc, staticIndex);
}

static void
cpBBTreeDestroy(cpBBTree *tree)
{
	cpHashSetFree(tree->leaves);
	
	if(tree->allocatedBuffers) cpArrayFreeEach(tree->allocatedBuffers, cpfree);
	cpArrayFree(tree->allocatedBuffers);
}

#pragma mark Insert/Remove

static void
cpBBTreeInsert(cpBBTree *tree, void *obj, cpHashValue hashid)
{
	Node *leaf = (Node *)cpHashSetInsert(tree->leaves, hashid, obj, tree, (cpHashSetTransFunc)leafSetTrans);
	
	Node *root = tree->root;
	tree->root = SubtreeInsert(root, leaf, tree);
	
	leaf->stamp = GetStamp(tree);
	LeafAddPairs(leaf, tree);
	IncrementStamp(tree);
}

static void
cpBBTreeRemove(cpBBTree *tree, void *obj, cpHashValue hashid)
{
	Node *leaf = (Node *)cpHashSetRemove(tree->leaves, hashid, obj);
	
	tree->root = SubtreeRemove(tree->root, leaf, tree);
	PairsClear(leaf, tree);
	NodeRecycle(tree, leaf);
}

static cpBool
cpBBTreeContains(cpBBTree *tree, void *obj, cpHashValue hashid)
{
	return (cpHashSetFind(tree->leaves, hashid, obj) != NULL);
}

#pragma mark Reindex

static void
cpBBTreeReindexQuery(cpBBTree *tree, cpSpatialIndexQueryFunc func, void *data)
{
	if(!tree->root) return;
	
	// LeafUpdate() may modify tree->root. Don't cache it.
	cpHashSetEach(tree->leaves, (cpHashSetIteratorFunc)LeafUpdate, tree);
	
	cpSpatialIndex *staticIndex = tree->spatialIndex.staticIndex;
	Node *staticRoot = (staticIndex && staticIndex->klass == Klass() ? ((cpBBTree *)staticIndex)->root : NULL);
	
	MarkContext context = {tree, staticRoot, func, data};
	MarkSubtree(tree->root, &context);
	if(staticIndex && !staticRoot) cpSpatialIndexCollideStatic((cpSpatialIndex *)tree, staticIndex, func, data);
	
	IncrementStamp(tree);
}

static void
cpBBTreeReindex(cpBBTree *tree)
{
	cpBBTreeReindexQuery(tree, VoidQueryFunc, NULL);
}

static void
cpBBTreeReindexObject(cpBBTree *tree, void *obj, cpHashValue hashid)
{
	Node *leaf = (Node *)cpHashSetFind(tree->leaves, hashid, obj);
	if(leaf){
		if(LeafUpdate(leaf, tree)) LeafAddPairs(leaf, tree);
		IncrementStamp(tree);
	}
}

#pragma mark Query

static void
cpBBTreePointQuery(cpBBTree *tree, cpVect point, cpSpatialIndexQueryFunc func, void *data)
{
	Node *root = tree->root;
	if(root) SubtreeQuery(root, &point, cpBBNew(point.x, point.y, point.x, point.y), func, data);
}

static void
cpBBTreeSegmentQuery(cpBBTree *tree, void *obj, cpVect a, cpVect b, cpFloat t_exit, cpSpatialIndexSegmentQueryFunc func, void *data)
{
	Node *root = tree->root;
	if(root) SubtreeSegmentQuery(root, obj, a, b, func, data);
}

static void
cpBBTreeQuery(cpBBTree *tree, void *obj, cpBB bb, cpSpatialIndexQueryFunc func, void *data)
{
	if(tree->root) SubtreeQuery(tree->root, obj, bb, func, data);
}

#pragma mark Misc

static int
cpBBTreeCount(cpBBTree *tree)
{
	return cpHashSetCount(tree->leaves);
}

typedef struct eachContext {
	cpSpatialIndexIteratorFunc func;
	void *data;
} eachContext;

static void each_helper(Node *node, eachContext *context){context->func(node->obj, context->data);}

static void
cpBBTreeEach(cpBBTree *tree, cpSpatialIndexIteratorFunc func, void *data)
{
	eachContext context = {func, data};
	cpHashSetEach(tree->leaves, (cpHashSetIteratorFunc)each_helper, &context);
}

static cpSpatialIndexClass klass = {
	(cpSpatialIndexDestroyImpl)cpBBTreeDestroy,
	
	(cpSpatialIndexCountImpl)cpBBTreeCount,
	(cpSpatialIndexEachImpl)cpBBTreeEach,
	
	(cpSpatialIndexContainsImpl)cpBBTreeContains,
	(cpSpatialIndexInsertImpl)cpBBTreeInsert,
	(cpSpatialIndexRemoveImpl)cpBBTreeRemove,
	
	(cpSpatialIndexReindexImpl)cpBBTreeReindex,
	(cpSpatialIndexReindexObjectImpl)cpBBTreeReindexObject,
	(cpSpatialIndexReindexQueryImpl)cpBBTreeReindexQuery,
	
	(cpSpatialIndexPointQueryImpl)cpBBTreePointQuery,
	(cpSpatialIndexSegmentQueryImpl)cpBBTreeSegmentQuery,
	(cpSpatialIndexQueryImpl)cpBBTreeQuery,
};

static inline cpSpatialIndexClass *Klass(){return &klass;}


#pragma mark Tree Optimization

static int
cpfcompare(const cpFloat *a, const cpFloat *b){
	return (*a < *b ? -1 : (*b < *a ? 1 : 0));
}

static void
fillNodeArray(Node *node, Node ***cursor){
	(**cursor) = node;
	(*cursor)++;
}

static Node *
partitionNodes(cpBBTree *tree, Node **nodes, int count)
{
	if(count == 1){
		return nodes[0];
	} else if(count == 2) {
		return NodeNew(tree, nodes[0], nodes[1]);
	}
	
	// Find the AABB for these nodes
	cpBB bb = nodes[0]->bb;
	for(int i=1; i<count; i++) bb = cpBBMerge(bb, nodes[i]->bb);
	
	// Split it on it's longest axis
	cpBool splitWidth = (bb.r - bb.l > bb.t - bb.b);
	
	// Sort the bounds and use the median as the splitting point
	cpFloat *bounds = (cpFloat *)cpcalloc(count*2, sizeof(cpFloat));
	if(splitWidth){
		for(int i=0; i<count; i++){
			bounds[2*i + 0] = nodes[i]->bb.l;
			bounds[2*i + 1] = nodes[i]->bb.r;
		}
	} else {
		for(int i=0; i<count; i++){
			bounds[2*i + 0] = nodes[i]->bb.b;
			bounds[2*i + 1] = nodes[i]->bb.t;
		}
	}
	
	qsort(bounds, count*2, sizeof(cpFloat), (int (*)(const void *, const void *))cpfcompare);
	cpFloat split = (bounds[count - 1] + bounds[count])*0.5f; // use the medain as the split
	cpfree(bounds);

	// Generate the child BBs
	cpBB a = bb, b = bb;
	if(splitWidth) a.r = b.l = split; else a.t = b.b = split;
	
	// Partition the nodes
	int right = count;
	for(int left=0; left < right;){
		Node *node = nodes[left];
		if(cpBBMergedArea(node->bb, b) < cpBBMergedArea(node->bb, a)){
//		if(cpBBProximity(node->bb, b) < cpBBProximity(node->bb, a)){
			right--;
			nodes[left] = nodes[right];
			nodes[right] = node;
		} else {
			left++;
		}
	}
	
	if(right == count){
		Node *node = NULL;
		for(int i=0; i<count; i++) node = SubtreeInsert(node, nodes[i], tree);
		return node;
	}
	
	// Recurse and build the node!
	return NodeNew(tree,
		partitionNodes(tree, nodes, right),
		partitionNodes(tree, nodes + right, count - right)
	);
}

//static void
//cpBBTreeOptimizeIncremental(cpBBTree *tree, int passes)
//{
//	for(int i=0; i<passes; i++){
//		Node *root = tree->root;
//		Node *node = root;
//		int bit = 0;
//		unsigned int path = tree->opath;
//		
//		while(!NodeIsLeaf(node)){
//			node = (path&(1<<bit) ? node->a : node->b);
//			bit = (bit + 1)&(sizeof(unsigned int)*8 - 1);
//		}
//		
//		root = subtreeRemove(root, node, tree);
//		tree->root = subtreeInsert(root, node, tree);
//	}
//}

void
cpBBTreeOptimize(cpSpatialIndex *index)
{
	if(index->klass != &klass){
		cpAssertWarn(cpFalse, "Ignoring cpBBTreeOptimize() call to non-tree spatial index.");
		return;
	}
	
	cpBBTree *tree = (cpBBTree *)index;
	Node *root = tree->root;
	if(!root) return;
	
	int count = cpBBTreeCount(tree);
	Node **nodes = (Node **)cpcalloc(count, sizeof(Node *));
	Node **cursor = nodes;
	
	cpHashSetEach(tree->leaves, (cpHashSetIteratorFunc)fillNodeArray, &cursor);
	
	SubtreeRecycle(tree, root);
	tree->root = partitionNodes(tree, nodes, count);
	cpfree(nodes);
}

#pragma mark Debug Draw

//#define CP_BBTREE_DEBUG_DRAW
#ifdef CP_BBTREE_DEBUG_DRAW
#include "OpenGL/gl.h"
#include "OpenGL/glu.h"
#include <GLUT/glut.h>

static void
NodeRender(Node *node, int depth)
{
	if(!NodeIsLeaf(node) && depth <= 10){
		NodeRender(node->a, depth + 1);
		NodeRender(node->b, depth + 1);
	}
	
	cpBB bb = node->bb;
	
//	GLfloat v = depth/2.0f;	
//	glColor3f(1.0f - v, v, 0.0f);
	glLineWidth(cpfmax(5.0f - depth, 1.0f));
	glBegin(GL_LINES); {
		glVertex2f(bb.l, bb.b);
		glVertex2f(bb.l, bb.t);
		
		glVertex2f(bb.l, bb.t);
		glVertex2f(bb.r, bb.t);
		
		glVertex2f(bb.r, bb.t);
		glVertex2f(bb.r, bb.b);
		
		glVertex2f(bb.r, bb.b);
		glVertex2f(bb.l, bb.b);
	}; glEnd();
}

void
cpBBTreeRenderDebug(cpSpatialIndex *index){
	if(index->klass != &klass){
		cpAssertWarn(cpFalse, "Ignoring cpBBTreeRenderDebug() call to non-tree spatial index.");
		return;
	}
	
	cpBBTree *tree = (cpBBTree *)index;
	if(tree->root) NodeRender(tree->root, 0);
}
#endif
