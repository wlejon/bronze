// Object-graph traversal, search, mutation, and cloning benchmark.
// Measures graph/tree traversal (DFS & BFS), property access,
// dynamic object allocation, subtree cloning, and node mutations.

import { measure } from './harness.js';

function makeLCG(seed) {
  let s = seed % 2147483647;
  if (s <= 0) s += 2147483646;
  return function () {
    s = (s * 16807) % 2147483647;
    return (s - 1) / 2147483646;
  };
}

class GraphNode {
  constructor(id, name, depth) {
    this.id = id;
    this.name = name;
    this.depth = depth;
    this.x = 0;
    this.y = 0;
    this.z = 0;
    this.worldX = 0;
    this.worldY = 0;
    this.worldZ = 0;
    this.flag = false;
    this.weight = 1.0;
    this.tags = [];
    this.children = [];
    this.parent = null;
  }

  addChild(child) {
    child.parent = this;
    this.children.push(child);
  }

  clone() {
    const copy = new GraphNode(this.id + 100000, this.name + '_copy', this.depth);
    copy.x = this.x;
    copy.y = this.y;
    copy.z = this.z;
    copy.flag = this.flag;
    copy.weight = this.weight;
    for (let i = 0; i < this.tags.length; i++) {
      copy.tags.push(this.tags[i]);
    }
    for (let i = 0; i < this.children.length; i++) {
      copy.addChild(this.children[i].clone());
    }
    return copy;
  }
}

function buildGraph(rng, totalNodes) {
  const root = new GraphNode(0, 'root', 0);
  const queue = [root];
  let id = 1;

  while (queue.length > 0 && id < totalNodes) {
    const parent = queue.shift();
    const childCount = 2 + Math.floor(rng() * 4); // 2 to 5 children

    for (let c = 0; c < childCount && id < totalNodes; c++) {
      const child = new GraphNode(id, 'node_' + id, parent.depth + 1);
      child.x = (rng() - 0.5) * 100;
      child.y = (rng() - 0.5) * 100;
      child.z = (rng() - 0.5) * 100;
      child.weight = 0.5 + rng() * 2.0;
      if (rng() > 0.5) child.tags.push('visible');
      if (rng() > 0.7) child.tags.push('collidable');
      if (rng() > 0.8) child.tags.push('dynamic');

      parent.addChild(child);
      queue.push(child);
      id++;
    }
  }
  return root;
}

function traverseDFS(node, parentWorldX, parentWorldY, parentWorldZ, acc) {
  node.worldX = parentWorldX + node.x * node.weight;
  node.worldY = parentWorldY + node.y * node.weight;
  node.worldZ = parentWorldZ + node.z * node.weight;

  acc.visited++;
  acc.sumX += node.worldX;
  acc.sumY += node.worldY;
  acc.sumZ += node.worldZ;

  for (let i = 0; i < node.children.length; i++) {
    traverseDFS(node.children[i], node.worldX, node.worldY, node.worldZ, acc);
  }
}

function searchBFS(root, targetTag) {
  const queue = [root];
  let matched = 0;

  while (queue.length > 0) {
    const curr = queue.shift();
    for (let t = 0; t < curr.tags.length; t++) {
      if (curr.tags[t] === targetTag) {
        matched++;
        curr.flag = !curr.flag;
        break;
      }
    }
    for (let i = 0; i < curr.children.length; i++) {
      queue.push(curr.children[i]);
    }
  }
  return matched;
}

function mutateGraph(node, delta) {
  node.x += Math.sin(delta + node.id) * 0.1;
  node.y += Math.cos(delta + node.id) * 0.1;
  node.z += Math.sin(delta * 0.5 + node.id) * 0.1;
  for (let i = 0; i < node.children.length; i++) {
    mutateGraph(node.children[i], delta);
  }
}

function runObjectGraphBench(passes) {
  const rng = makeLCG(42);
  const root = buildGraph(rng, 1500);

  let totalVisited = 0;
  let totalMatches = 0;
  let finalSum = 0;

  for (let pass = 0; pass < passes; pass++) {
    mutateGraph(root, pass * 0.05);

    const acc = { visited: 0, sumX: 0, sumY: 0, sumZ: 0 };
    traverseDFS(root, 0, 0, 0, acc);
    totalVisited += acc.visited;
    finalSum += acc.sumX + acc.sumY + acc.sumZ;

    totalMatches += searchBFS(root, 'visible');
    totalMatches += searchBFS(root, 'collidable');

    // Clone a branch and traverse it
    if (pass % 5 === 0 && root.children.length > 0) {
      const clonedBranch = root.children[0].clone();
      const cloneAcc = { visited: 0, sumX: 0, sumY: 0, sumZ: 0 };
      traverseDFS(clonedBranch, 0, 0, 0, cloneAcc);
      finalSum += cloneAcc.sumX * 0.01;
    }
  }

  const checksum = Math.round(totalVisited + totalMatches + finalSum);
  return checksum;
}

console.log('object_graph checksum=' +
  measure('object_graph', () => runObjectGraphBench(100)));
