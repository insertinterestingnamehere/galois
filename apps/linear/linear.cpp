#include "Galois/Galois.h"
#include "Galois/Statistic.h"
#include "Galois/Graphs/Graph.h"
#include "llvm/Support/CommandLine.h"
#include "Lonestar/BoilerPlate.h"

#include <cmath>
#include <iostream>
#include <algorithm>

namespace cll = llvm::cl;

static const char* name = "Iterative methods";
static const char* desc = "Iterative methods for solving PD linear systems Ax=b\n";
static const char* url = 0;

static cll::opt<int> algo("algo", cll::desc("Node to start search from"), cll::init(0));
static cll::opt<int> N(cll::Positional, cll::desc("<N>"), cll::Required);
static cll::opt<int> sparsity(cll::Positional, cll::desc("<nonzeros>"), cll::Required);
static cll::opt<int> seed(cll::Positional, cll::desc("<seed>"), cll::Required);

static const double TOL = 1e-10;
static int MaxIterations;

struct BaseNode {
  double x;  // Estimate of x
  double b;  
  double actual; // Actual value of x
  double weight; // A_ii 
  BaseNode(double _b, double a, double w): x(0), b(_b), actual(a), weight(w) { }
};

template<typename Graph>
static double residual(Graph& g) {
  double retval = 0;
  for (typename Graph::active_iterator ii = g.active_begin(), ei = g.active_end(); ii != ei; ++ii) {
    double r = ii->getData().x - ii->getData().actual;
    retval += r * r;
  }
  return retval;
}

template<typename Graph>
static double relativeResidual(Graph& g) {
  double retval = 0;
  for (typename Graph::active_iterator ii = g.active_begin(), ei = g.active_end(); ii != ei; ++ii) {
    double r = ii->getData().x - ii->getData().x_prev;
    retval += r * r;
  }
  return retval;
}

/*
 * Jacobi 
 *   x_i = (b_i - \sum_j A_ij * x_j)/A_ii
 */
struct Jacobi {
  struct Node: public BaseNode {
    double x_prev;
    Node(double b, double a, double w): BaseNode(b, a, w) { }
  };

  typedef Galois::Graph::FirstGraph<Node,double,true> Graph;

  Graph& graph;

  Jacobi(Graph& g): graph(g) { }

  void operator()(const Graph::GraphNode& src) {
    Node& node = src.getData(Galois::ALL);
    node.x_prev = node.x;

    double sum = 0;
    for (Graph::neighbor_iterator dst = graph.neighbor_begin(src, Galois::ALL),
        edst = graph.neighbor_end(src, Galois::ALL); dst != edst; ++dst) {
      assert(src != *dst);
      double weight = graph.getEdgeData(src, dst, Galois::NONE);
      sum += weight * dst->getData(Galois::NONE).x;
    }
    node.x = (node.b - sum) / node.weight;
  }

  void operator()() {
    for (int i = 0; i < MaxIterations; ++i) {
      std::for_each(graph.active_begin(), graph.active_end(), *this);
      double r = relativeResidual(graph);
      std::cout << "RE " << r << "\n";
      if (r < TOL)
        return;
    }
    std::cout << "Did not converge\n";
  }
};



/*
 * from http://en.wikipedia.org/wiki/Conjugate_gradient_method
 *
 * function [x] = conjgrad(A,b,x)
 *   r=b-A*x; ///DIST
 *   p=r;     //SER
 *   rsold=r'*r;  //SER
 *   for i=1:size(A,1)
 *       Ap=A*p;
 *       alpha=rsold/(p'*Ap);
 *       x=x+alpha*p;
 *       r=r-alpha*Ap;
 *       rsnew=r'*r;
 *       if sqrt(rsnew)<1e-10
 *             break;
 *       end
 *       p=r+rsnew/rsold*p;
 *       rsold=rsnew;
 *   end
 * end
 */
struct ConjugateGradient {
  struct Node: public BaseNode {
    double r;
    double p;
    double ap;
    Node(double b, double a, double w): BaseNode(b, a, w), r(b), p(b) { }
  };

  typedef Galois::Graph::FirstGraph<Node,double,true> Graph;

  Graph& graph;

  ConjugateGradient(Graph& g): graph(g) { }

  void operator()() {
    // rsold = r'*r
    double rs_old = 0.0;
    for (Graph::active_iterator ii = graph.active_begin(), ei = graph.active_end(); ii != ei; ++ii) {
      double r = ii->getData(Galois::NONE).r;
      rs_old += r * r;
    }

    for (int iteration = 0; iteration < MaxIterations; ++iteration) {
      // Ap = A*p
      for (Graph::active_iterator src = graph.active_begin(), 
          esrc = graph.active_end(); src != esrc; ++src) {
        Node& node = src->getData(Galois::NONE);
        node.ap = 0;
        for (Graph::neighbor_iterator dst = graph.neighbor_begin(*src, Galois::ALL),
            edst = graph.neighbor_end(*src, Galois::ALL); dst != edst; ++dst) {
          double weight = graph.getEdgeData(*src, dst, Galois::NONE); 
          node.ap += weight * dst->getData(Galois::NONE).p;
        }
        node.ap += node.weight * node.p;
      }

      // alpha = rs_old/(p'*Ap)
      double sum = 0;
      for (Graph::active_iterator src = graph.active_begin(), 
          esrc = graph.active_end(); src != esrc; ++src) {
        Node& node = src->getData(Galois::NONE);
        sum += node.ap * node.p;
      }
      double alpha = rs_old / sum;

      // x = x + alpha*p
      // r = r - alpha*Ap
      // rs_new = r'*r
      double rs_new = 0;
      for (Graph::active_iterator src = graph.active_begin(),
          esrc = graph.active_end(); src != esrc; ++src) {
        Node& node = src->getData(Galois::NONE);
        node.x += alpha * node.p;
        node.r -= alpha * node.ap;
        rs_new += node.r * node.r;
      }

      // if (sqrt(rs_new) < 1e-10) break
      double r = sqrt(rs_new);
      if (r < TOL)
        break;

      std::cout << "RE " << r << "\n";

      // p = r + rs_new/rs_old * p
      for (Graph::active_iterator src = graph.active_begin(),
          esrc = graph.active_end(); src != esrc; ++src) {
        Node& node = src->getData(Galois::NONE);
        node.p = node.r + rs_new/rs_old * node.p;
      }

      // rs_old = rs_new
      rs_old = rs_new;
    }
  }
};

#if 0
/**
 * GRAPHLAB implementation of Gaussiabn Belief Propagation Code See
 * algrithm description and explanation in: Danny Bickson, Gaussian
 * Belief Propagation: Theory and Application. Ph.D. Thesis. The
 * Hebrew University of Jerusalem. Submitted October 2008.
 * http://arxiv.org/abs/0811.2518 By Danny Bickson, CMU. Send any bug
 * fixes/reports to bickson@cs.cmu.edu Code adapted to GraphLab by
 * Joey Gonzalez, CMU July 2010
 *
 * Functionality: The code solves the linear system Ax = b using
 * Gaussian Belief Propagation. (A is either square matrix or
 * skinny). A assumed to be full column rank.  Algorithm is described
 * in Algorithm 1, page 14 of the above Phd Thesis.
 *
 * If you are using this code, you should cite the above reference. Thanks!
 */
struct GBP {
  struct Node: public BaseNode {
    double prev_x;
    double prior_mean;
    double prev_prec;
    double cur_prec;
    Node(double b, double a, double w): BaseNode(b, a, w),
      prior_mean(b), prev_prec(w), cur_prec(0) { }
  };

  struct Edge {
    double weight;
    double mean;
    double prec;
    Edge() { } // Graph requires this
    Edge(double w): weight(w), mean(0), prec(0) { }
  };
  
  typedef Galois::Graph::FirstGraph<Node,Edge,true> Graph;

  Graph& graph;

  GBP(Graph& g): graph(g) { }

  void operator()(const Graph::GraphNode& src) {
    Node& node = src.getData(Galois::NONE);

    node.prev_x = node.x;
    node.prev_prec = node.cur_prec;

    double mu_i = node.prior_mean;
    double J_i = node.weight;
    assert(J_i != 0);

    // Incoming edges
    for (Graph::neighbor_iterator dst = graph.neighbor_begin(src, Galois::ALL),
        edst = graph.neighbor_end(src, Galois::ALL); dst != edst; ++dst) {
      const Edge& edge = graph.getEdgeData(*dst, src, Galois::NONE);
      mu_i += edge.mean;
      J_i += edge.prec;
    }

    assert(J_i != 0);
    node.x = mu_i / J_i;
    assert(!isnan(node.x));
    node.cur_prec = J_i;

    for (Graph::neighbor_iterator dst = graph.neighbor_begin(src, Galois::NONE),
        edst = graph.neighbor_end(src, Galois::NONE); dst != edst; ++dst) {
      Edge& inEdge = graph.getEdgeData(*dst, src, Galois::NONE);
      Edge& outEdge = graph.getEdgeData(src, dst, Galois::NONE);

      double mu_i_j = mu_i - inEdge.mean;
      double J_i_j = J_i - inEdge.prec;

      outEdge.mean = -(outEdge.weight * mu_i_j / J_i_j);
      outEdge.prec = -((outEdge.weight * outEdge.weight) / J_i_j);

      //double priority = fabs(node.cur_prec) + 1e-5;

      //ctx.push(*dst);
    }
  }

  void operator()() {
    for (int i = 0; i < MaxIterations; ++i) {
      std::for_each(graph.active_begin(), graph.active_end(), *this);
      double r = relativeResidual(graph);
      std::cout << "RE " << r << "\n";
      if (r < TOL)
        return;
    }
    std::cout << "Did not converge\n";
  }
};
#else
// From asynch_GBP.m in gabp-src.zip at
//  http://www.cs.cmu.edu/~bickson/gabp/index.html
struct GBP {
  struct Node: public BaseNode {
    double x_prev;
    double mean; // h(i)
    double prec; // J(i)
    Node(double b, double a, double w): BaseNode(b, a, w),
       prec(0) { }
  };

  struct Edge {
    double weight; // A(i,i)
    double mean;   // Mh = zeros(m, m)
    double prec;   // MJ = zeros(m, m)
    Edge() { } // FirstGraph requires this
    Edge(double w): weight(w), mean(0), prec(0) { }
  };
  
  typedef Galois::Graph::FirstGraph<Node,Edge,true> Graph;

  Graph& graph;

  GBP(Graph& g): graph(g) { }

  void operator()(const Graph::GraphNode& src) {
    Node& node = src.getData(Galois::NONE);
    
    node.x_prev = node.x;

    node.mean = node.b;
    node.prec = node.weight;

    // Sum up all mean and percision values got from neighbors
    //  h(i) = b(i) + sum(Mh(:,i));  %(7)
    // Variance can not be zero (must be a diagonally dominant matrix)!
    //  assert(A(i,i) ~= 0);
    //  J(i) = A(i,i) + sum(MJ(:,i));
    for (Graph::neighbor_iterator dst = graph.neighbor_begin(src, Galois::ALL),
        edst = graph.neighbor_end(src, Galois::ALL); dst != edst; ++dst) {
      const Edge& edge = graph.getEdgeData(*dst, src, Galois::NONE);
      node.mean += edge.mean;
      node.prec += edge.prec;
    }

    node.x = node.mean / node.prec;

    // Send message to all neighbors
    //  for j=1:m
    //    if (i ~= j && A(i,j) ~= 0)
    //      h_j = h(i) - Mh(j,i);
    //      J_j = J(i) - MJ(j,i);
    //      assert(A(i,j) == A(j,i));
    //      assert(J_j ~= 0);
    //      Mh(i,j) = (-A(j,i) / J_j)* h_j;
    //      MJ(i,j) = (-A(j,i) / J_j) * A(i,j);
    //    end
    //  end
    for (Graph::neighbor_iterator dst = graph.neighbor_begin(src, Galois::NONE),
        edst = graph.neighbor_end(src, Galois::NONE); dst != edst; ++dst) {
      Edge& inEdge = graph.getEdgeData(*dst, src, Galois::NONE);
      Edge& outEdge = graph.getEdgeData(src, dst, Galois::NONE);
      
      double mean_j = node.mean - inEdge.mean;
      double prec_j = node.prec - inEdge.prec;

      outEdge.mean = -inEdge.weight * mean_j / prec_j;
      outEdge.prec = -inEdge.weight * outEdge.weight / prec_j;
      assert(inEdge.weight == outEdge.weight);

      //double priority = fabs(node.cur_prec) + 1e-5;

      //ctx.push(*dst);
    }
  }

  void operator()() {
    std::vector<Graph::GraphNode> elements(graph.size());
    std::copy(graph.active_begin(), graph.active_end(), elements.begin());

    for (int i = 0; i < MaxIterations; ++i) {
      std::random_shuffle(elements.begin(), elements.end());
      std::for_each(elements.begin(), elements.end(), *this);
      double r = relativeResidual(graph);
      std::cout << "RE " << r << "\n";
      if (r < TOL)
        return;
    }
    std::cout << "Did not converge\n";
  }
};
#endif

// XXX: Improve 
struct Cholesky {
  struct Node: public BaseNode {
    int id;
    Node(double b, double a, double w): BaseNode(b, a, w) { }
  };

  typedef Galois::Graph::FirstGraph<Node,double,true> Graph;

  Graph& graph;

  Cholesky(Graph& g): graph(g) { }

  void operator()() {
    std::vector<Graph::GraphNode> A(graph.size());
    std::copy(graph.active_begin(), graph.active_end(), A.begin());

    //for (int k = 0; k < N; k++) {
    //  A[k][k] = Math.sqrt(A[k][k]);
    //  for (int i = k + 1; i < N; i++) {
    //    A[i][k] = A[i][k] / A[k][k];
    //  }
    //  for (int j = k + 1; j < N; j++) {
    //    for (int i = j; i < N; i++) {
    //      A[i][j] -= A[i][k] * A[j][k];
    //    }
    //  }
    //}
  }
};


//! Generate a symmetric, positive definite sparse matrix
template<typename Graph>
struct GenerateInput {
  struct Node {
    size_t id;
    double x;
    double b;
    Node(double _x): id(0), x(_x), b(0) { }
  };

  typedef Galois::Graph::FirstGraph<Node,double,true> GenGraph;

  GenerateInput(Graph& g, int N, int sparsity, int seed) {
    srand(seed);

    GenGraph g1;
    generate(g1, N, sparsity);

    copy(g1, g);
  }

  //! Generate random SPD matrix (as well as solution to Ax=b)
  void generate(GenGraph& g1, int N, int sparsity) {
    typedef typename GenGraph::GraphNode GraphNode;
    
    // Generate band matrix, L
    //
    // A11  0   0
    // A21 A22  0
    // A31 A32 A33
    // 
    // stored as:
    //
    //  0   0  A11
    //  0  A21 A22
    // A31 A32 A33
    std::vector<double> L(N*sparsity);
    for (int i = 0; i < N; ++i) {
      for (int j = 0; j < sparsity; ++j) {
        double w = rand() / (double) RAND_MAX;
        //if (j == sparsity - 1) {
        //  w += 1;
        //}
        L[i*sparsity+j] = w;
      }
    }
    for (int i = 0; i < sparsity - 1; ++i) {
      for (int j = 0; j < sparsity - 1 - i; ++j) {
#if 0
        std::cout << "(" << i << "," << j << ")\n";
#endif
        L[i*sparsity+j] = 0;
      }
    }

    // Compute lower-triangle of L*L'
    std::vector<double> LL(N*sparsity);
    for (int i = 0; i < N; ++i) {
      for (int j = 0; j < sparsity; ++j) {
        if (i-j < 0)
          continue;

        for (int k = j; k < sparsity; ++k) {
#if 0
          std::cout << "LL(" << i << "," << sparsity-1-j
            << ") += L(" << i-j << "," << k
            << ") * L(" << i << "," << k - j << ")\n";
#endif
          LL[i*sparsity+sparsity-1-j] += L[(i-j)*sparsity+k] * L[i*sparsity+k-j];
        }
      }
    }

    for (int i = 0; i < N; ++i) {
      LL[i*sparsity+sparsity-1] += 1;
    }

    // Create nodes
    std::vector<GraphNode> nodes;
    for (int i = 0; i < N; ++i) {
      double x = rand() / (double) RAND_MAX;
      GraphNode n = g1.createNode(Node(x));
      g1.addNode(n);
      nodes.push_back(n);
    }
    
    // Shuffle 'em
    std::random_shuffle(nodes.begin(), nodes.end());

    // Create edges
    int nnz = 0;
    for (int i = 0; i < N; ++i) {
      GraphNode n = nodes[i];
      for (int j = 0; j < sparsity; ++j) {
        double& entry = LL[i*sparsity+j];
        if (entry) {
          g1.addEdge(nodes[i], nodes[i-sparsity+j+1], entry);
          g1.addEdge(nodes[i-sparsity+j+1], nodes[i], entry);
          nnz += 2;
        }
      }
    }

    std::cout << "N: " << N << " nnz: " << nnz << "\n";

    // Solve system Ax = b
    for (typename GenGraph::active_iterator src = g1.active_begin(), esrc = g1.active_end();
        src != esrc; ++src) {
      Node& node = src->getData();

      // node.b = 0;
      for (typename GenGraph::neighbor_iterator dst = g1.neighbor_begin(*src), 
          edst = g1.neighbor_end(*src); dst != edst; ++dst) {
        node.b += g1.getEdgeData(*src, dst) * dst->getData().x;
      }
    }
  }

  //! Copy from GenGraph representation to user Graph
  void copy(GenGraph& g1, Graph& g) {
    typedef typename Graph::GraphNode GraphNode;
    typedef typename Graph::node_type node_type;
    typedef typename Graph::edge_type edge_type;

    // Create nodes
    std::vector<GraphNode> nodes;
    size_t id = 0;
    for (typename GenGraph::active_iterator ii = g1.active_begin(), ei = g1.active_end();
        ii != ei; ++ii) {
      ii->getData().id = id++;
      double w = g1.getEdgeData(*ii, *ii);
      Node& node = ii->getData();
      GraphNode n = g.createNode(node_type(node.b, node.x, w));
      g.addNode(n);
      nodes.push_back(n);
    }

    // Create edges
    for (typename GenGraph::active_iterator src = g1.active_begin(), esrc = g1.active_end();
        src != esrc; ++src) {
      GraphNode snode = nodes[src->getData().id];

      for (typename GenGraph::neighbor_iterator dst = g1.neighbor_begin(*src),
          edst = g1.neighbor_end(*src); dst != edst; ++dst) {
        // A_ii handled as node.weight
        if (*src == *dst)
          continue;
        GraphNode dnode = nodes[dst->getData().id];
        edge_type edge(g1.getEdgeData(*src, dst));

        g.addEdge(snode, dnode, edge);
      }
    }
  }
};

template<typename Algo>
static void start(int N, int sparsity, int seed) {
  typename Algo::Graph g;
  GenerateInput<typename Algo::Graph>(g, N, sparsity, seed);
  
  Galois::StatTimer T;
  T.start();
  Algo algo(g);
  algo();
  T.stop();

  std::cout << "Residual is: " << residual(g) << "\n";
}

int main(int argc, char **argv) {
  LonestarStart(argc, argv, std::cout, name, desc, url);

  MaxIterations = N;

  switch (algo) {
    // TODO add cholesky
    case 2: std::cout << "Using GBP\n"; start<GBP>(N, sparsity, seed); break;
    case 1: std::cout << "Using CG\n"; start<ConjugateGradient>(N, sparsity, seed); break;
    case 0: 
    default: std::cout << "Using Jacobi\n"; start<Jacobi>(N, sparsity, seed); break;
  }
 
  return 0;
}
