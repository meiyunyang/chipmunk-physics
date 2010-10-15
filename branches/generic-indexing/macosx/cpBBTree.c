#include "stdlib.h"
#include "stdio.h"

#include "chipmunk.h"

#pragma mark Node Functions

typedef struct cpBBTreeNode {
	void *obj;
	cpBB bb;
	struct cpBBTreeNode *a, *b, *parent;
} cpBBTreeNode;

//static cpBBTreeNode *
//cpBBTreeNodeAlloc(void)
//{
//	return (cpBBTreeNode *)cpcalloc(1, sizeof(cpBBTreeNode));
//}

static inline void
recycleNode(cpBBTree *tree, cpBBTreeNode *node)
{
	node->parent = tree->pooledNodes;
	tree->pooledNodes = node;
}

static cpBBTreeNode *
getFreeNode(cpBBTree *tree)
{
	cpBBTreeNode *node = tree->pooledNodes;
	
	if(node){
		tree->pooledNodes = node->parent;
		return node;
	} else {
		// Pool is exhausted, make more
		int count = CP_BUFFER_BYTES/sizeof(cpBBTreeNode);
		cpAssert(count, "Buffer size is too small.");
		
		cpBBTreeNode *buffer = (cpBBTreeNode *)cpmalloc(CP_BUFFER_BYTES);
		cpArrayPush(tree->allocatedBuffers, buffer);
		
		// push all but the first one, return the first instead
		for(int i=1; i<count; i++) recycleNode(tree, buffer + i);
		return buffer;
	}
}

static cpBBTreeNode *
cpBBTreeNodeNewLeaf(cpBBTree *tree, void *obj, cpBB bb)
{
	cpBBTreeNode *node = getFreeNode(tree);
	node->obj = obj;
	node->bb = bb;
	
	node->a = node->b = node->parent = NULL;
	
	return node;
}

static inline void
cpBBTreeNodeSetA(cpBBTreeNode *node, cpBBTreeNode *value)
{
	node->a = value;
	value->parent = node;
}

static inline void
cpBBTreeNodeSetB(cpBBTreeNode *node, cpBBTreeNode *value)
{
	node->b = value;
	value->parent = node;
}

static cpBBTreeNode *
cpBBTreeNodeInit(cpBBTreeNode *node, cpBBTreeNode *a, cpBBTreeNode *b)
{
	node->obj = NULL;
	node->bb = cpBBmerge(a->bb, b->bb);
	node->parent = NULL;
	
	cpBBTreeNodeSetA(node, a);
	cpBBTreeNodeSetB(node, b);
	
	return node;
}

static inline cpBool
cpBBTreeNodeIsLeaf(cpBBTreeNode *node)
{
	return (node->obj != NULL);
}

static cpFloat inline
cpBBMergedArea(cpBB a, cpBB b)
{
	return (cpfmax(a.r, b.r) - cpfmin(a.l, b.l))*(cpfmax(a.t, b.t) - cpfmin(a.b, b.b));
}

static cpFloat inline
cpBBArea(cpBB bb)
{
	return (bb.r - bb.l)*(bb.t - bb.b);
}

static cpBBTreeNode *
cpBBTreeNodeInsert(cpBBTree *tree, cpBBTreeNode *node, cpBBTreeNode *insert)
{
	if(cpBBTreeNodeIsLeaf(node)){
		return cpBBTreeNodeInit(getFreeNode(tree), insert, node);
	} else {
		cpFloat area_a = cpBBArea(node->a->bb) + cpBBMergedArea(node->b->bb, insert->bb);
		cpFloat area_b = cpBBArea(node->b->bb) + cpBBMergedArea(node->a->bb, insert->bb);
		
		if(area_a < area_b){
			cpBBTreeNodeSetB(node, cpBBTreeNodeInsert(tree, node->b, insert));
		} else {
			cpBBTreeNodeSetA(node, cpBBTreeNodeInsert(tree, node->a, insert));
		}
		
		node->bb = cpBBmerge(node->bb, insert->bb);
		return node;
	}
}

static void
cpBBTreeNodeQuery(cpBBTreeNode *node, void *obj, cpBB bb, cpSpatialIndexQueryCallback func, void *data)
{
//	if(cpBBintersects(bb, node->bb)){
//		if(cpBBTreeNodeIsLeaf(node)){
//			func(obj, node->obj, data);
//		} else {
//			cpBBTreeNodeQuery(node->a, obj, bb, func, data);
//			cpBBTreeNodeQuery(node->b, obj, bb, func, data);
//		}
//	}
	
	cpBBTreeNode *nodeStack[200];
	nodeStack[0] = node;
	int top = 1;
	while(top){
		cpBBTreeNode *node = nodeStack[--top];
		if(cpBBintersects(bb, node->bb)){
			if(cpBBTreeNodeIsLeaf(node)){
				func(obj, node->obj, data);
			} else {
				nodeStack[top++] = node->b;
				nodeStack[top++] = node->a;
			}
		}
	}
}

static inline cpBBTreeNode *
cpBBTreeNodeOther(cpBBTreeNode *node, cpBBTreeNode *child)
{
	return (node->a == child ? node->b : node->a);
}

static inline void
cpBBTreeNodeReplaceChild(cpBBTreeNode *node, cpBBTreeNode *child, cpBBTreeNode *value)
{
	if(node->a == child){
		cpBBTreeNodeSetA(node, value);
	} else {
		cpBBTreeNodeSetB(node, value);
	}
	
	for(; node; node = node->parent){
		node->bb = cpBBmerge(node->a->bb, node->b->bb);
	}
}

static int imax(int a, int b){return (a > b ? a : b);}

static int
cpBBTreeNodeDepth(cpBBTreeNode *node)
{
	return (cpBBTreeNodeIsLeaf(node) ? 1 : 1 + imax(cpBBTreeNodeDepth(node->a), cpBBTreeNodeDepth(node->b)));
}

#pragma mark Memory Management Functions

cpBBTree *
cpBBTreeAlloc(void)
{
	return (cpBBTree *)cpcalloc(1, sizeof(cpBBTree));
}

static cpSpatialIndexClass klass;

static int leafSetEql(void *obj, cpBBTreeNode *node){return (obj == node->obj);}

static void *
leafSetTrans(void *obj, cpBBTree *tree)
{
	return cpBBTreeNodeNewLeaf(tree, obj, tree->bbfunc(obj));
}



cpBBTree *
cpBBTreeInit(cpBBTree *tree, cpSpatialIndexBBFunc bbfunc)
{
	tree->spatialIndex.klass = &klass;
	
	tree->bbfunc = bbfunc;
	tree->leaves = cpHashSetNew(0, (cpHashSetEqlFunc)leafSetEql, (cpHashSetTransFunc)leafSetTrans);
	tree->root = NULL;
	
	tree->pooledNodes = NULL;
	tree->allocatedBuffers = cpArrayNew(0);
	
	return tree;
}

cpBBTree *
cpBBTreeNew(cpSpatialIndexBBFunc bbfunc)
{
	return cpBBTreeInit(cpBBTreeAlloc(), bbfunc);
}

static void
freeSubTree(cpBBTree *tree, cpBBTreeNode *node)
{
	if(!cpBBTreeNodeIsLeaf(node)){
		freeSubTree(tree, node->a);
		freeSubTree(tree, node->b);
		recycleNode(tree, node);
	}
}

static void freeWrap(void *ptr, void *unused){cpfree(ptr);}

static void
cpBBTreeDestroy(cpBBTree *tree)
{
	cpHashSetFree(tree->leaves);
	cpArrayEach(tree->allocatedBuffers, freeWrap, NULL);
}

#pragma mark Insert/Remove

static inline void
insertLeaf(cpBBTreeNode *node, cpBBTree *tree)
{
	tree->root = (tree->root ? cpBBTreeNodeInsert(tree, tree->root, node) : node);
}

static void
cpBBTreeInsert(cpBBTree *tree, void *obj, cpHashValue hashid)
{
	cpBBTreeNode *node = cpHashSetInsert(tree->leaves, hashid, obj, tree);
	insertLeaf(node, tree);
}

static void
cpBBTreeRemove(cpBBTree *tree, void *obj, cpHashValue hashid)
{
	cpBBTreeNode *root = tree->root;
	cpBBTreeNode *node = cpHashSetRemove(tree->leaves, hashid, obj);
	
	if(node == root){
		tree->root = NULL;
	} else {
		cpBBTreeNode *parent = node->parent;
		if(parent == root){
			tree->root = cpBBTreeNodeOther(root, node);
			tree->root->parent = NULL;
		} else {
			cpBBTreeNodeReplaceChild(parent->parent, parent, cpBBTreeNodeOther(parent, node));
		}
	}
	
	recycleNode(tree, node);
}

static cpBool
cpBBTreeContains(cpBBTree *tree, void *obj, cpHashValue hashid)
{
	return (cpHashSetFind(tree->leaves, hashid, obj) != NULL);
}

#pragma mark Reindex



static void
cpBBTreeReindex(cpBBTree *tree)
{
	if(tree->root) freeSubTree(tree, tree->root);
	tree->root = NULL;
	
	cpHashSetEach(tree->leaves, (cpHashSetIterFunc)insertLeaf, tree);
	
//	printf("tree depth %d\n", cpBBTreeDepth(tree));
}

static int
cpBBTreeReindexObject(cpBBTree *tree, void *obj, cpHashValue hashid)
{
	cpAssert(cpFalse, "TODO Not implemented");
	return cpTrue;
}

#pragma mark Query

static void
cpBBTreePointQuery(cpBBTree *tree, cpVect point, cpSpatialIndexQueryCallback func, void *data)
{
	if(tree->root) cpBBTreeNodeQuery(tree->root, &point, cpBBNew(point.x, point.y, point.x, point.y), func, data);
}

static void
cpBBTreeSegmentQuery(cpBBTree *tree, void *obj, cpVect a, cpVect b, cpFloat t_exit, cpSpatialIndexSegmentQueryCallback func, void *data)
{
	cpAssert(cpFalse, "TODO Not implemented");
}

static void
cpBBTreeQuery(cpBBTree *tree, void *obj, cpBB bb, cpSpatialIndexQueryCallback func, void *data)
{
	if(tree->root) cpBBTreeNodeQuery(tree->root, obj, bb, func, data);
}

static int
cpBBTreeDepth(cpBBTree *tree)
{
	return (tree->root ? cpBBTreeNodeDepth(tree->root) : 0);
}

typedef struct queryInsertContext {
	cpBBTree *tree;
	cpSpatialIndexQueryCallback func;
	void *data;
} queryInsertContext;


static void
queryInsertLeafHelper(cpBBTreeNode *node, queryInsertContext *context)
{
	cpBBTree *tree = context->tree;
	void *obj = node->obj;
	
	cpBB bb = node->bb = tree->bbfunc(obj);
	cpBBTreeQuery(tree, obj, bb, context->func, context->data);
	insertLeaf(node, tree);
}

static cpBBTreeNode *
queryInsertRebuild(cpBBTree *tree, cpBBTreeNode *node, cpBBTreeNode *root, cpSpatialIndexQueryCallback func, void *data)
{
	if(cpBBTreeNodeIsLeaf(node)){
		void *obj = node->obj;
		cpBB bb = node->bb = tree->bbfunc(obj);
		
		if(root){
			cpBBTreeNodeQuery(root, obj, bb, func, data);
			return cpBBTreeNodeInsert(tree, root, node);
		} else {
			return node;
		}
	} else {
		root = queryInsertRebuild(tree, node->b, root, func, data);
		root = queryInsertRebuild(tree, node->a, root, func, data);
		recycleNode(tree, node);
		return root;
	}
}

static void
cpBBTreeReindexQuery(cpBBTree *tree, cpSpatialIndexQueryCallback func, void *data)
{
//	if(tree->root) freeSubTree(tree, tree->root);
//	tree->root = NULL;
//	
//	queryInsertContext context = {tree, func, data};
//	cpHashSetEach(tree->leaves, (cpHashSetIterFunc)queryInsertLeafHelper, &context);
	
	if(tree->root)
		tree->root = queryInsertRebuild(tree, tree->root, NULL, func, data);
	
//	printf("tree depth % 5d for % 5d objects\n", cpBBTreeDepth(tree), tree->leaves->entries);
}

#pragma mark Misc

static int
cpBBTreeCount(cpBBTree *tree)
{
	return tree->leaves->entries;
}

typedef struct eachContext {
	cpSpatialIndexIterator func;
	void *data;
} eachContext;

static void eachHelper(cpHandle *hand, eachContext *context){context->func(hand->obj, context->data);}

static void
cpBBTreeEach(cpBBTree *tree, cpSpatialIndexIterator func, void *data)
{
	eachContext context = {func, data};
	cpHashSetEach(tree->leaves, (cpHashSetIterFunc)eachHelper, &context);
}

static cpSpatialIndexClass klass = {
	(cpSpatialIndexDestroyFunc)cpBBTreeDestroy,
	
	(cpSpatialIndexCountFunc)cpBBTreeCount,
	(cpSpatialIndexEachFunc)cpBBTreeEach,
	
	(cpSpatialIndexContainsFunc)cpBBTreeContains,
	(cpSpatialIndexInsertFunc)cpBBTreeInsert,
	(cpSpatialIndexRemoveFunc)cpBBTreeRemove,
	
	(cpSpatialIndexReindexFunc)cpBBTreeReindex,
	(cpSpatialIndexReindexObjectFunc)cpBBTreeReindexObject,
	
	(cpSpatialIndexPointQueryFunc)cpBBTreePointQuery,
	(cpSpatialIndexSegmentQueryFunc)cpBBTreeSegmentQuery,
	(cpSpatialIndexQueryFunc)cpBBTreeQuery,
	(cpSpatialIndexReindexQueryFunc)cpBBTreeReindexQuery,
};
