/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2020, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */
#ifndef GALOIS_FMM_DIST_PULL
#define GALOIS_FMM_DIST_PULL
#endif

#include <galois/Galois.h>
#include <galois/graphs/FileGraph.h>
#include <galois/graphs/Graph.h>

// Vendored from an old version of LLVM for Lonestar app command line handling.
#include "llvm/Support/CommandLine.h"

#include "DistBench/Start.h"

#ifdef GALOIS_ENABLE_VTUNE
#include "galois/runtime/Profile.h"
#endif

constexpr static char const* name = "Fast Marching Method";
constexpr static char const* desc =
    "Eikonal equation solver "
    "(https://en.wikipedia.org/wiki/Fast_marching_method)";
constexpr static char const* url         = "";
constexpr static char const* REGION_NAME = "FMM";

#define DIM_LIMIT 2 // 2-D specific
using data_t         = double;
constexpr data_t INF = std::numeric_limits<double>::max();

enum Algo { serial = 0, parallel };
enum SourceType { scatter = 0, analytical };

const char* const ALGO_NAMES[] = {"serial", "parallel"};

static llvm::cl::OptionCategory catAlgo("1. Algorithmic Options");
static llvm::cl::opt<Algo>
    algo("algo", llvm::cl::value_desc("algo"),
         llvm::cl::desc("Choose an algorithm (default parallel):"),
         llvm::cl::values(clEnumVal(serial, "serial heap implementation"),
                          clEnumVal(parallel, "parallel implementation")),
         llvm::cl::init(parallel), llvm::cl::cat(catAlgo));
static llvm::cl::opt<unsigned> RF{"rf",
                                  llvm::cl::desc("round-off factor for OBIM"),
                                  llvm::cl::init(0u), llvm::cl::cat(catAlgo)};
static llvm::cl::opt<double> tolerance("e", llvm::cl::desc("Final error bound"),
                                       llvm::cl::init(2.e-6),
                                       llvm::cl::cat(catAlgo));

static llvm::cl::OptionCategory catInput("2. Input Options");
static llvm::cl::opt<SourceType> source_type(
    "sourceFormat", llvm::cl::desc("Choose an source format:"),
    llvm::cl::values(clEnumVal(scatter, "a set of discretized points"),
                     clEnumVal(analytical, "boundary in a analytical form")),
    llvm::cl::init(scatter), llvm::cl::cat(catInput));
static llvm::cl::opt<std::string> input_segy(
    "segy", llvm::cl::value_desc("path-to-file"),
    llvm::cl::desc("Use SEG-Y (rev 1) file as input speed map. NOTE: This will "
                   "determine the size on each dimensions"),
    llvm::cl::init(""), llvm::cl::cat(catInput));
static llvm::cl::opt<std::string> input_npy(
    "inpy", llvm::cl::value_desc("path-to-file"),
    llvm::cl::desc(
        "Use npy file (dtype=float32) as input speed map. NOTE: This will "
        "determine the size on each dimensions"),
    llvm::cl::init(""), llvm::cl::cat(catInput));
static llvm::cl::opt<std::string> input_csv(
    "icsv", llvm::cl::value_desc("path-to-file"),
    llvm::cl::desc(
        "Use csv file as input speed map. NOTE: Current implementation "
        "requires explicit definition of the size on each dimensions (see -d)"),
    llvm::cl::init(""), llvm::cl::cat(catInput));
// TODO parameterize the following
static data_t xa = -.5, xb = .5;
static data_t ya = -.5, yb = .5;

static llvm::cl::OptionCategory catOutput("3. Output Options");
static llvm::cl::opt<std::string>
    output_csv("ocsv", llvm::cl::desc("Export results to a csv format file"),
               llvm::cl::init(""), llvm::cl::cat(catOutput));
static llvm::cl::opt<std::string>
    output_npy("onpy", llvm::cl::desc("Export results to a npy format file"),
               llvm::cl::init(""), llvm::cl::cat(catOutput));

static llvm::cl::OptionCategory catDisc("4. Discretization options");
namespace internal {
template <typename T>
struct StrConv;
template <>
struct StrConv<std::size_t> {
  auto operator()(const char* str, char** endptr, int base) {
    return strtoul(str, endptr, base);
  }
};
template <>
struct StrConv<double> {
  auto operator()(const char* str, char** endptr, int) {
    return strtod(str, endptr);
  }
};
} // namespace internal
template <typename NumTy, int MAX_SIZE = 0>
struct NumVecParser : public llvm::cl::parser<std::vector<NumTy>> {
  template <typename... Args>
  NumVecParser(Args&... args) : llvm::cl::parser<std::vector<NumTy>>(args...) {}
  internal::StrConv<NumTy> strconv;
  // parse - Return true on error.
  bool parse(llvm::cl::Option& O, llvm::StringRef ArgName,
             const std::string& ArgValue, std::vector<NumTy>& Val) {
    const char* beg = ArgValue.c_str();
    char* end;

    do {
      NumTy d = strconv(Val.empty() ? beg : end + 1, &end, 0);
      if (!d)
        return O.error("Invalid option value '" + ArgName + "=" + ArgValue +
                       "': should be comma-separated unsigned integers");
      Val.push_back(d);
    } while (*end == ',');
    if (*end != '\0')
      return O.error("Invalid option value '" + ArgName + "=" + ArgValue +
                     "': should be comma-separated unsigned integers");
    if (MAX_SIZE && Val.size() > MAX_SIZE)
      return O.error(ArgName + "=" + ArgValue + ": expect no more than " +
                     std::to_string(MAX_SIZE) + " numbers but get " +
                     std::to_string(Val.size()));
    return false;
  }
};
static llvm::cl::opt<std::vector<std::size_t>, false,
                     NumVecParser<std::size_t, DIM_LIMIT>>
    dims("d", llvm::cl::value_desc("d1,d2"),
         llvm::cl::desc("Size of each dimensions as a comma-separated array "
                        "(support up to 2-D)"),
         llvm::cl::cat(catDisc));
static llvm::cl::opt<std::vector<double>, false,
                     NumVecParser<double, DIM_LIMIT>>
    intervals(
        "dx", llvm::cl::value_desc("dx,dy"),
        llvm::cl::desc("Interval of each dimensions as a comma-separated array "
                       "(support up to 2-D)"),
        llvm::cl::init(std::vector<double>{1., 1.}), llvm::cl::cat(catDisc));

static uint64_t nx, ny;
static std::size_t NUM_CELLS;
static data_t dx, dy;
#include "distributed/DgIO.h"

///////////////////////////////////////////////////////////////////////////////

#include "fastmarchingmethod.h"

// No fine-grained locks built into the graph.
// Use atomics for ALL THE THINGS!
struct NodeData {
  data_t speed; // read only
  data_t solution;
};
galois::DynamicBitSet bitset_solution;

using Graph = galois::graphs::DistGraph<NodeData, void>;
using GNode = Graph::GraphNode;
using BL    = galois::InsertBag<GNode>;
std::unique_ptr<galois::graphs::GluonSubstrate<Graph>> syncSubstrate;

#include "distributed/fmm_sync.h"
#include "structured/grids.h"
#include "structured/utils.h"

#include "util/input.hh"

template <typename Graph, typename BL,
          typename GNode = typename Graph::GraphNode,
          typename T     = typename BL::value_type>
void AssignBoundary(Graph& graph, BL& boundary) {

  if (source_type == scatter) {
    GNode g_n = xy2id({0., 0.});
    if (graph.isLocal(g_n))
      boundary.push(graph.getLID(g_n));
    else
      galois::gDebug("not on this host");
  } else {
    const auto& allNodes = graph.allNodesRange();
    galois::do_all(
        galois::iterate(allNodes.begin(), allNodes.end()),
        [&](T node) noexcept {
          if (graph.getGID(node) >= NUM_CELLS)
            return;

          auto [x, y] = id2xy(graph.getGID(node));
          if (NonNegativeRegion(double2d_t{x, y})) {
            if (!NonNegativeRegion(double2d_t{x + dx, y}) ||
                !NonNegativeRegion(double2d_t{x - dx, y}) ||
                !NonNegativeRegion(double2d_t{x, y + dy}) ||
                !NonNegativeRegion(double2d_t{x, y - dy})) {
              boundary.push(node);
            }
          }
        },
        galois::loopname("assignBoundary"));
  }
}

/////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////

static void initCells(Graph& graph) {
  const auto& all_nodes = graph.allNodesRange();
  galois::do_all(
      galois::iterate(all_nodes.begin(), all_nodes.end()),
      [&](GNode node) noexcept {
        auto& node_data    = graph.getData(node);
        node_data.solution = INF;
        assert(graph.getGID(node) >= NUM_CELLS || node_data.speed > 0);
      },
      galois::no_stats(),
      galois::loopname(
          syncSubstrate->get_run_identifier("initializeCells").c_str()));
}

static void initBoundary(Graph& graph, BL& boundary) {
  galois::do_all(
      galois::iterate(boundary.begin(), boundary.end()),
      [&](GNode b) noexcept {
        auto& boundary_data    = graph.getData(b);
        boundary_data.solution = BoundaryCondition(id2xy(graph.getGID(b)));
      },
      galois::no_stats(),
      galois::loopname(
          syncSubstrate->get_run_identifier("initializeBoundary").c_str()));
}

////////////////////////////////////////////////////////////////////////////////

template <typename Graph, typename It = typename Graph::edge_iterator>
bool pullUpdate(Graph& graph, data_t& up_sln, It dir) {

  bool didWork = false;

  auto uni_pull = [&](It ei) {
    GNode neighbor = graph.getEdgeDst(ei);
    if (graph.getGID(neighbor) >= NUM_CELLS)
      return;
    auto& n_data = graph.getData(neighbor);
    if (data_t n_sln = n_data.solution; n_sln < up_sln) {
      up_sln  = n_sln;
      didWork = true;
    }
  };

  // check one neighbor
  uni_pull(dir++);
  // check the other neighbor
  uni_pull(dir);
  return didWork;
}

////////////////////////////////////////////////////////////////////////////////
// Solver

template <typename Graph, typename NodeData, typename It>
data_t solveQuadratic(Graph& graph, NodeData& my_data, It edge_begin,
                      It edge_end) {
  assert(edge_end - edge_begin == DIM_LIMIT * 2);
  data_t sln         = my_data.solution;
  const data_t speed = my_data.speed;

  std::array<std::pair<data_t, data_t>, DIM_LIMIT> sln_delta = {
      std::make_pair(sln, dx), std::make_pair(sln, dy)};

  int non_zero_counter = 0;
  auto dir             = edge_begin;
  for (auto& [s, d] : sln_delta) {
    if (dir >= edge_end)
      GALOIS_DIE("Dimension exceeded");

    if (pullUpdate(graph, s, dir)) {
      non_zero_counter++;
    }
    std::advance(dir, 2);
  }
  // local computation, nothing about graph
  if (non_zero_counter == 0)
    return sln;
  // DGDEBUG("solveQuadratic: #upwind_dirs: ", non_zero_counter);

  std::sort(sln_delta.begin(), sln_delta.end(),
            [&](std::pair<data_t, data_t>& a, std::pair<data_t, data_t>& b) {
              return a.first < b.first;
            });
  auto p = sln_delta.begin();
  data_t a(0.), b_(0.), c_(0.), f(1. / (speed * speed));
  do {
    const auto& [s, d] = *p++;
    // DGDEBUG(s, " ", d);
    // Arrival time may be updated in previous rounds
    // in which case remaining upwind dirs become invalid
    if (sln < s)
      break;

    double temp = 1. / (d * d);
    a += temp;
    temp *= s;
    b_ += temp;
    temp *= s;
    c_ += temp;
    data_t b = -2. * b_, c = c_ - f;
    DGDEBUG("tabc: ", temp, " ", a, " ", b, " ", c);

    double del = b * b - (4. * a * c);
    DGDEBUG(a, " ", b, " ", c, " del=", del);
    if (del >= 0) {
      double new_sln = (-b + std::sqrt(del)) / (2. * a);
      galois::gDebug("new solution: ", new_sln);
      if (new_sln > s) { // conform causality
        sln = std::min(sln, new_sln);
      }
    }
  } while (--non_zero_counter);
  return sln;
}
////////////////////////////////////////////////////////////////////////////////
// FMM

void FastMarching(Graph& graph) {
  using DGTerminatorDetector = galois::DGAccumulator<uint32_t>;
  DGTerminatorDetector more_work;
  unsigned _round_counter = 0;

  const auto& nodes_with_edges = graph.allNodesWithEdgesRange();

#ifdef GALOIS_ENABLE_VTUNE
  galois::runtime::profileVtune(
      [&]() {
#endif
        do {
#ifndef NDEBUG
          sleep(5); // Debug pause
          galois::gDebug("\n********\n");
#endif
          syncSubstrate->set_num_round(_round_counter);
          more_work.reset();
          galois::do_all(
              galois::iterate(nodes_with_edges),
              [&](GNode node) {
                if (graph.getGID(node) >= NUM_CELLS)
                  return;
                auto& node_data = graph.getData(node);
#ifndef NDEBUG
                auto [i, j] = id2ij(graph.getGID(node));
        // DGDEBUG("Processing ", node, " (g", graph.getGID(node),
        //         (node < graph.numMasters() ? "M" : "m"), ") (", i, " ",
        //         j, ") sln:", node_data.solution);
#endif
                data_t sln_temp =
                    solveQuadratic(graph, node_data, graph.edge_begin(node),
                                   graph.edge_end(node));
                if (data_t old_sln = galois::min(node_data.solution, sln_temp);
                    sln_temp < old_sln) {
                  bitset_solution.set(node);
                  more_work += 1;
#ifndef NDEBUG
                  DGDEBUG("update ", node, " (g", graph.getGID(node),
                          (node < graph.numMasters() ? "M" : "m"), ") (", i,
                          " ", j, ") with ", sln_temp);
                } else {
          // DGDEBUG(node, " solution not updated: ", sln_temp,
          //         " (currently ", node_data.solution, ")");
#endif
                }
              },
              galois::no_stats(),
              galois::steal(), // galois::wl<OBIM>(Indexer),
              galois::loopname(
                  syncSubstrate->get_run_identifier("Pull").c_str()));

          syncSubstrate->sync<writeSource, readDestination, Reduce_min_solution,
                              Bitset_solution>("FastMarching");

          galois::runtime::reportStat_Tsum(
              REGION_NAME,
              "NumWorkItems_" + (syncSubstrate->get_run_identifier()),
              (uint32_t)more_work.read_local());
          ++_round_counter;
        } while (more_work.reduce(syncSubstrate->get_run_identifier().c_str()));
#ifdef GALOIS_ENABLE_VTUNE
      },
      "FMM_VTune");
#endif
}

////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
// Sanity check

void SanityCheck(Graph& graph) {
  galois::DGReduceMax<double> max_error;

  const auto& masterNodes = graph.masterNodesRange();
  galois::do_all(
      galois::iterate(masterNodes.begin(), masterNodes.end()),
      [&](GNode node) noexcept {
        if (graph.getGID(node) >= NUM_CELLS)
          return;
        auto& my_data = graph.getData(node);
        if (my_data.solution == INF) {
          auto [ii, jj] = id2ij(graph.getGID(node));
          galois::gPrint("Untouched cell: ", node, " (g", graph.getGID(node),
                         ") ", (node < graph.numMasters() ? "M" : "m"), " (",
                         ii, " ", jj, ")\n");
          return;
        }

        data_t new_val = solveQuadratic(graph, my_data, graph.edge_begin(node),
                                        graph.edge_end(node));
        if (data_t old_val = my_data.solution; new_val != old_val) {
          data_t error = std::abs(new_val - old_val) / std::abs(old_val);
          max_error.update(error);
          if (error > tolerance) {
            auto [ii, jj] = id2ij(graph.getGID(node));
            galois::gPrint("Error bound violated at cell ", node, " (", ii, " ",
                           jj, "): old_val=", old_val, " new_val=", new_val,
                           " error=", error, "\n");
          }
        }
      },
      galois::no_stats(), galois::loopname("sanityCheck"));

  auto me = max_error.reduce();
  DGPRINT("max err: ", me, "\n");
}

template <typename Graph, typename GNode = typename Graph::GraphNode>
void SanityCheck2(Graph& graph) {
  galois::do_all(
      galois::iterate(0ul, NUM_CELLS),
      [&](GNode node) noexcept {
        auto [x, y]    = id2xy(graph.getGID(node));
        auto& solution = graph.getData(node).solution;
        assert(std::abs(solution - std::sqrt(x * x + y * y)));
      },
      galois::no_stats(), galois::loopname("sanityCheck2"));
}

////////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv) noexcept {
  galois::DistMemSys galois_system;
  DistBenchStart(argc, argv, name, desc, url);

  galois::gDebug(ALGO_NAMES[algo]);

  SetKnobs(dims);

  galois::StatTimer Ttotal("TimerTotal");
  Ttotal.start();

  // generate grids
  std::unique_ptr<Graph> graph;
  std::tie(graph, syncSubstrate) = distGraphInitialization<NodeData, void>();

  // _debug_print();

  // initialize all cells
  SetupGrids(*graph);
  initCells(*graph);
  galois::runtime::getHostBarrier().wait();

  // TODO better way for boundary settings?
  BL boundary;
  AssignBoundary(*graph, boundary);
#ifndef NDEBUG
  // print boundary
  galois::gDebug("vvvvvvvv boundary vvvvvvvv");
  for (GNode b : boundary) {
    auto [x, y] = id2xy(b);
    galois::gDebug(b, " (", x, ", ", y, ")");
  }
  DGDEBUG("^^^^^^^^ boundary ^^^^^^^^");
#endif

  bitset_solution.resize(graph->size());
  galois::runtime::getHostBarrier().wait();

  for (int run = 0; run < numRuns; ++run) {
    DGPRINT("Run ", run, " started\n");
    std::string tn = "Timer_" + std::to_string(run);
    galois::StatTimer Tmain(tn.c_str());

    galois::DGAccumulator<uint32_t> busy;
    busy.reset();
    if (!boundary.empty()) {
      busy += 1;
#ifndef NDEBUG
      // print boundary
      for (GNode b : boundary) {
        auto [ii, jj] = id2ij(graph->getGID(b));
        DGDEBUG("boundary: ", b, "(g", graph->getGID(b),
                (b < graph->numMasters() ? "M" : "m"), ") (", ii, " ", jj,
                ") with ", graph->getData(b).solution);
      }
#endif
      initBoundary(*graph, boundary);
    } else {
      DGDEBUG("No boundary element");
    }
    assert(busy.reduce() && "Boundary not defined!");

    Tmain.start();

    FastMarching(*graph);

    Tmain.stop();

    galois::runtime::getHostBarrier().wait();
    SanityCheck(*graph);
    // SanityCheck2(graph);

    if ((run + 1) != numRuns) {
      galois::runtime::getHostBarrier().wait();
      bitset_solution.reset();

      initCells(*graph);
      galois::runtime::getHostBarrier().wait();
    }
  }

  Ttotal.stop();
  return 0;
}
