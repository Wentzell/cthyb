/*******************************************************************************
 *
 * TRIQS: a Toolbox for Research in Interacting Quantum Systems
 *
 * Copyright (C) 2014, P. Seth, I. Krivenko, M. Ferrero and O. Parcollet
 *
 * TRIQS is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * TRIQS is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * TRIQS. If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/
#pragma once
#include "./configuration.hpp"
#include "./atom_diag.hpp"
#include "./solve_parameters.hpp"
#include "triqs/utility/rbt.hpp"
#include "triqs/statistics/histograms.hpp"
//#define PRINT_CONF_DEBUG

using namespace triqs;
using triqs::utility::rb_tree;
using triqs::utility::rbt_insert_error;

namespace cthyb {

/********************************************
 Calculate the trace of the impurity problem.
 ********************************************/
class impurity_trace {

 bool use_norm_as_weight;
 bool measure_density_matrix;

 public:

 using trace_t = double;
 // using trace_t = std::complex<double>; TODO

 // construct from the config, the diagonalization of h_loc, and parameters
 impurity_trace(configuration& c, atom_diag const& h_diag, solve_parameters_t const& p);

 ~impurity_trace() {
  cancel_insert_impl(); // in case of an exception, we need to remove any trial nodes before cleaning the tree!
 }

 std::pair<double, trace_t> compute(double p_yee = -1, double u_yee = 0);

 // ------- Configuration and h_loc data ----------------

 const configuration* config;                                  // config object does exist longer (temporally) than this object.
 const atom_diag* h_diag;                                      // access to the diagonalization of h_loc
 const int n_orbitals = h_diag->get_fops().size();             // total number of orbital flavours
 const int n_blocks = h_diag->n_blocks();                      //
 const int n_eigstates = h_diag->get_full_hilbert_space_dim(); // size of the hilbert space

 // ------- Trace data ----------------

 private:

 struct bool_and_matrix {
  bool is_valid;
  matrix<double> mat;
 };
 arrays::vector<bool_and_matrix> density_matrix; // density_matrix, by block, with a bool to say if it has been recomputed
 arrays::vector<bool_and_matrix> atomic_rho;     // atomic density matrix (non-normalized)
 double atomic_z;                                // atomic partition function
 double atomic_norm;                             // Frobenius norm of atomic_rho

 public:
 arrays::vector<bool_and_matrix> const& get_density_matrix() const { return density_matrix; }

 // ------------------ Cache data ----------------

 private:
 // The data stored for each node in tree
 struct cache_t {
  double dtau_l = 0, dtau_r = 0; // difference in tau of this node and left and right sub-trees
  std::vector<int> block_table; // number of blocks limited to 2^15
  std::vector<arrays::matrix<double>> matrices; // partial product of operator/time evolution matrices
  std::vector<double> matrix_lnorms; // -ln(norm(matrix))
  std::vector<bool> matrix_norm_valid; // is the norm of the matrix still valid?
  cache_t(int n_blocks) : block_table(n_blocks), matrix_lnorms(n_blocks), matrices(n_blocks), matrix_norm_valid(n_blocks) {}
 };

 struct node_data_t {
  op_desc op;
  cache_t cache;
  node_data_t(op_desc op, int n_blocks) : op(op), cache(n_blocks) {}
  void reset(op_desc op_new) { op = op_new; }
 };

 using rb_tree_t = rb_tree<time_pt, node_data_t, std::greater<time_pt>>;
 using node = rb_tree_t::node;

#ifdef EXT_DEBUG
 public:
#endif
 rb_tree_t tree;       // the red black tree and its nodes

 // ---------------- Cache machinery ----------------
 void update_cache();

 private:

 // The dimension of block b
 int get_block_dim(int b) const { return h_diag->get_block_dim(b);}

 // the i-th eigenvalue of the block b
 double get_block_eigenval(int b, int i) const { return h_diag->get_eigenvalue(b,i); }

 // the minimal eigenvalue of the block b
 double get_block_emin(int b) const { return get_block_eigenval(b, 0); }

 // node, block -> image of the block by n->op (the operator)
 int get_op_block_map(node n, int b) const {
  return (n->op.dagger ? h_diag->cdag_connection(n->op.linear_index, b) : h_diag->c_connection(n->op.linear_index, b));
 }

 // the matrix of n->op, from block b to its image
 matrix<double> const& get_op_block_matrix(node n, int b) const {
  return (n->op.dagger ? h_diag->cdag_matrix(n->op.linear_index, b) : h_diag->c_matrix(n->op.linear_index, b));
 }

 // recursive function for tree traversal
 int compute_block_table(node n, int b);
 std::pair<int, double> compute_block_table_and_bound(node n, int b, double bound_threshold, bool use_threshold = true);
 std::pair<int, arrays::matrix<double>> compute_matrix(node n, int b);

 void update_cache_impl(node n);
 void update_dtau(node n);

 bool use_norm_of_matrices_in_cache = true; // When a matrix is computed in cache, its spectral radius replaces the norm estimate

 // integrity check
 void check_cache_integrity(bool print = false);
 void check_cache_integrity_one_node(node n, bool print);
 int check_one_block_table_linear(node n, int b, bool print); // compare block table to that of a linear method (ie. no tree)
 matrix<double> check_one_block_matrix_linear(node n, int b, bool print); // compare matrix to that of a linear method (ie. no tree)

 // Pool of detached nodes
 class nodes_storage {

  const int n_blocks;
  std::vector<node> nodes;
  int i;

  // make a new detached black node
  node make_new_node() {
   return new rb_tree_t::node_t(time_pt{}, node_data_t{{}, n_blocks}, false, 1);
  }

 public:
  inline nodes_storage(int n_blocks, int size = 0) : n_blocks(n_blocks), i(-1) {
   for(int j = 0; j < size; ++j) nodes.push_back(make_new_node());
  }
  inline ~nodes_storage() { for(auto & n : nodes) delete n; }

  // Change the number of stored nodes
  inline void reserve(int size) {
    if(size > nodes.size())
     for(int j = nodes.size(); j < size; ++j) nodes.push_back(make_new_node());
  }
  inline int index() const { return i; }
  inline bool is_index_reset() const { return i == -1; }
  inline int reset_index() { int i_ = i; i = -1; return i_; }
  inline node swap_next(node n) { std::swap(n,nodes[++i]); return n; }
  inline node swap_prev(node n) { std::swap(n,nodes[i--]); return n; }
  inline node take_next() { return nodes[++i]; }
  inline node take_prev() { return nodes[i--]; }
 };

 public:

 /*************************************************************************
  *  Ordinary binary search tree (BST) insertion of the trial nodes
  *************************************************************************/
 // We have a set of trial nodes, which we can glue, un-glue in the tree at will.
 // This avoids allocations.

 int tree_size = 0; // size of the tree +/- the added/deleted node

 // a pool of trial nodes, ready to be glued in the tree. Max 4 to allow for double insertions
 nodes_storage trial_nodes = {n_blocks,4};

 // for each inserted node, need to know {parent_of_node,child_is_left}
 std::vector<std::pair<node, bool>> inserted_nodes = {{nullptr, false}, {nullptr, false}, {nullptr, false}, {nullptr, false}};

 node try_insert_impl(node h, node n) { // implementation
  if (h == nullptr) return n;
  if (h->key == n->key) throw rbt_insert_error{};
  auto smaller = tree.get_comparator()(n->key, h->key);
  if (smaller)
   h->left = try_insert_impl(h->left, n);
  else
   h->right = try_insert_impl(h->right, n);
  if (inserted_nodes[trial_nodes.index()].first == nullptr) inserted_nodes[trial_nodes.index()] = {h, smaller};
  h->modified = true;
  return h;
 }

 // unlink all glued trial nodes
 void cancel_insert_impl() {
  for (int i = 0; i <= trial_nodes.index(); ++i) {
   auto& r = inserted_nodes[i];
   if (r.first != nullptr) (r.second ? r.first->left : r.first->right) = nullptr;
  }
  if (tree_size == trial_nodes.index() + 1) tree.get_root() = nullptr;
 }

 /*************************************************************************
  * Node Insertion
  *************************************************************************/

 public:
 // Put a trial node at tau for operator op using an ordinary BST insertion (ie. not red black)
 void try_insert(time_pt const& tau, op_desc const& op) {
  if (trial_nodes.index() > 3) TRIQS_RUNTIME_ERROR << "Error : more than 4 insertions ";
  auto& root = tree.get_root();
  node n = trial_nodes.take_next(); // get the next available node
  inserted_nodes[trial_nodes.index()] = {nullptr, false};
  n->reset(tau, op);                            // change the time and op of the node
  root = try_insert_impl(root, n);              // insert it using a regular BST, no red black
  tree_size++;
 }

 // Remove all trial nodes from the tree
 void cancel_insert() {
  cancel_insert_impl();
  trial_nodes.reset_index();
  tree_size = tree.size();
  tree.clear_modified();
  check_cache_integrity();
 }

 // confirm the insertion of the nodes, with red black balance
 void confirm_insert() {
  cancel_insert_impl();                         // remove BST inserted nodes
  // then reinsert the nodes in in balanced RBT
  int imax = trial_nodes.reset_index();
  for (int i = 0; i <= imax; ++i) {
    node n = trial_nodes.take_next();
    tree.insert(n->key, {n->op, n_blocks});
  }
  trial_nodes.reset_index();
  update_cache();
  tree_size = tree.size();
  tree.clear_modified();
  check_cache_integrity();
 }

 /*************************************************************************
  * Node Removal
  *************************************************************************/
 private:
 std::vector<node> removed_nodes;
 std::vector<time_pt> removed_keys;

 public:
 // Find and mark as deleted the nth operator with fixed dagger and block_index
 // n=0 : first operator, n=1, second, etc...
 time_pt try_delete(int n, int block_index, bool dagger) noexcept {
  // traverse the tree, looking for the nth operator of the correct dagger, block_index
  int i = 0;
  node x = find_if(tree, [&](node no) {
   if (no->op.dagger == dagger && no->op.block_index == block_index) ++i;
   return i == n + 1;
  });
  removed_nodes.push_back(x);             // store the node
  removed_keys.push_back(x->key);         // store the key
  tree.set_modified_from_root_to(x->key); // mark all nodes on path from node to root as modified
  x->delete_flag = true;                  // mark the node for deletion
  tree_size--;
  return x->key;
 }

 // Clean all the delete flags
 void cancel_delete() {
  for (auto& n : removed_nodes) n->delete_flag = false;
  removed_nodes.clear();
  removed_keys.clear();
  tree_size = tree.size();
  tree.clear_modified();
  check_cache_integrity();
 }

 // Confirm deletion: the nodes flagged for deletion are truly deleted
 void confirm_delete() {
  for (auto& k : removed_keys) tree.delete_node(k); // CANNOT use the node here
  removed_nodes.clear();
  removed_keys.clear();
  update_cache();
  tree_size = tree.size();
  tree.clear_modified();
  check_cache_integrity();
 }

 /*************************************************************************
  * Node shift (=insertion+deletion)
  *************************************************************************/

 // No try_shift implemented. Use combination of try_insert and try_delete instead.

 // Cancel the shift
 void cancel_shift() {

  // Inserted nodes
  cancel_insert_impl();
  trial_nodes.reset_index();

  // Deleted nodes
  for (auto& n : removed_nodes) n->delete_flag = false;
  removed_nodes.clear();
  removed_keys.clear();

  tree_size = tree.size();
  tree.clear_modified();
  check_cache_integrity();
 }

 // Confirm the shift of the node, with red black balance
 void confirm_shift() {

  // Inserted nodes
  cancel_insert_impl();                         //  first remove BST inserted nodes
  //  then reinsert the nodes used for real in rb tree
  int imax = trial_nodes.reset_index();
  for (int i = 0; i <= imax; ++i) {
    node n = trial_nodes.take_next();
    tree.insert(n->key, {n->op, n_blocks});
  }
  trial_nodes.reset_index();

  // Deleted nodes
  for (auto& k : removed_keys) tree.delete_node(k); // CANNOT use the node here
  removed_nodes.clear();
  removed_keys.clear();

  // update cache only at the end
  update_cache();
  tree_size = tree.size();
  tree.clear_modified();
  check_cache_integrity();
 }

 /*************************************************************************
  * Node replacement (replace op_desc according to a substitution table)
  *************************************************************************/
 private:
 // Store copies of the nodes to be replaced
 nodes_storage backup_nodes = {n_blocks};

 node try_replace_impl(node n, std::vector<op_desc> const& substitute_c,
                               std::vector<op_desc> const& substitute_c_dag) noexcept {

  node new_left = nullptr, new_right = nullptr;
  if(n->left) new_left = try_replace_impl(n->left,substitute_c,substitute_c_dag);
  if(n->right) new_right = try_replace_impl(n->right,substitute_c,substitute_c_dag);

  auto const& op = n->op;
  auto const& new_op = (op.dagger ? substitute_c_dag : substitute_c)[op.linear_index];
  bool op_changed = new_op.linear_index != op.linear_index;
  node new_node = n;
  if(op_changed || new_left != n->left || new_right != n->right) {
   auto key = n->key;
   auto color = n->color;
   auto N = n->N;

   new_node = backup_nodes.swap_next(n);
   if(op_changed) new_node->reset(key,new_op);
   else           new_node->reset(key,op);
   new_node->left = new_left;
   new_node->right = new_right;
   new_node->color = color;
   new_node->N = N;
   new_node->modified = true;
  }
  return new_node;
 }

 node cancel_replace_impl(node n) {
  node n_in_tree = n;
  if(n_in_tree && n_in_tree->modified) n = backup_nodes.swap_prev(n);
  if(n_in_tree->right) cancel_replace_impl(n_in_tree->right);
  if(n_in_tree->left) cancel_replace_impl(n_in_tree->left);
  return n;
 }

 public:
 void try_replace(std::vector<op_desc> const& substitute_c,
                  std::vector<op_desc> const& substitute_c_dag) noexcept {
  if(tree_size == 0) return;

  if (!backup_nodes.is_index_reset()) TRIQS_RUNTIME_ERROR << "impurity_trace: improper use of try_replace()";
  backup_nodes.reserve(tree.size());
  auto& root = tree.get_root();
  root = try_replace_impl(root,substitute_c,substitute_c_dag);
 }

 void confirm_replace() {
  backup_nodes.reset_index();
  update_cache();
  tree.clear_modified();
  check_cache_integrity();
 }

 void cancel_replace() {
  if(tree_size ==0 || backup_nodes.is_index_reset()) return;
  auto& root = tree.get_root();
  root = cancel_replace_impl(root);
  check_cache_integrity();
 }

 private:
 // ---------------- Histograms ----------------
 struct histograms_t {

  histograms_t(int n_subspaces) : n_subspaces(n_subspaces) {};
  int n_subspaces;

  // How many block non zero at root of the tree
  statistics::histogram n_block_at_root = {n_subspaces, "histo_n_block_at_root.dat"};

  // how many block kept after the truncation with the bound
  statistics::histogram n_block_kept = {n_subspaces, "histo_n_block_kept.dat"};

  // What is the dominant block in the trace computation ? Sorted by number or energy
  statistics::histogram dominant_block_bound = {n_subspaces, "histo_dominant_block_bound.dat"};
  statistics::histogram dominant_block_trace = {n_subspaces, "histo_dominant_block_trace.dat"};
  statistics::histogram_segment_bin dominant_block_energy_bound = {0, 100, 100, "histo_dominant_block_energy_bound.dat"};
  statistics::histogram_segment_bin dominant_block_energy_trace = {0, 100, 100, "histo_dominant_block_energy_trace.dat"};

  // Various ratios : trace/bound, trace/first term of the trace, etc..
  statistics::histogram_segment_bin trace_over_norm= {0, 1.5, 100, "histo_trace_over_norm.dat"};
  statistics::histogram_segment_bin trace_abs_over_norm = {0, 1.5, 100, "histo_trace_abs_over_norm.dat"};
  statistics::histogram_segment_bin trace_over_trace_abs = {0, 1.5, 100, "histo_trace_over_trace_abs.dat"};
  statistics::histogram_segment_bin trace_over_bound = {0, 1.5, 100, "histo_trace_over_bound.dat"};
  statistics::histogram_segment_bin trace_first_over_sec_term = {0, 1.0, 100, "histo_trace_first_over_sec_term.dat"};
  statistics::histogram_segment_bin trace_first_term_trace = {0, 1.0, 100, "histo_trace_first_term_trace.dat"};
 };
 std::unique_ptr<histograms_t> histo;
};
}
