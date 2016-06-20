/*++
Copyright (c) 2016 Microsoft Corporation

Module Name:

    model_based_opt.cpp

Abstract:

    Model-based optimization for linear real arithmetic.

Author:

    Nikolaj Bjorner (nbjorner) 2016-27-4

Revision History:


--*/

#include "model_based_opt.h"
#include "uint_set.h"

std::ostream& operator<<(std::ostream& out, opt::ineq_type ie) {
    switch (ie) {
    case opt::t_eq: return out << " = ";
    case opt::t_lt: return out << " < ";
    case opt::t_le: return out << " <= ";        
    }
    return out;
}


namespace opt {
    

    model_based_opt::model_based_opt()
    {
        m_rows.push_back(row());
    }

        
    bool model_based_opt::invariant() {
        for (unsigned i = 0; i < m_rows.size(); ++i) {
            if (!invariant(i, m_rows[i])) {
                return false;
            }
        }
        return true;
    }

#define PASSERT(_e_) if (!(_e_)) { TRACE("opt", display(tout, r);); SASSERT(_e_); }

    bool model_based_opt::invariant(unsigned index, row const& r) {
        vector<var> const& vars = r.m_vars;
        for (unsigned i = 0; i < vars.size(); ++i) {
            // variables in each row are sorted and have non-zero coefficients
            SASSERT(i + 1 == vars.size() || vars[i].m_id < vars[i+1].m_id);
            SASSERT(!vars[i].m_coeff.is_zero());
        }
        
        PASSERT(r.m_value == get_row_value(r));
        PASSERT(r.m_type != t_eq ||  r.m_value.is_zero());
        // values satisfy constraints
        PASSERT(index == 0 || r.m_type != t_lt ||  r.m_value.is_neg());
        PASSERT(index == 0 || r.m_type != t_le || !r.m_value.is_pos());        
        return true;
    }
        
    // a1*x + obj 
    // a2*x + t2 <= 0
    // a3*x + t3 <= 0
    // a4*x + t4 <= 0
    // a1 > 0, a2 > 0, a3 > 0, a4 < 0
    // x <= -t2/a2
    // x <= -t2/a3
    // determine lub among these.
    // then resolve lub with others
    // e.g., -t2/a2 <= -t3/a3, then 
    // replace inequality a3*x + t3 <= 0 by -t2/a2 + t3/a3 <= 0
    // mark a4 as invalid.
    // 

    // a1 < 0, a2 < 0, a3 < 0, a4 > 0
    // x >= t2/a2
    // x >= t3/a3
    // determine glb among these
    // the resolve glb with others.
    // e.g. t2/a2 >= t3/a3
    // then replace a3*x + t3 by t3/a3 - t2/a2 <= 0
    // 
    inf_eps model_based_opt::maximize() {
        SASSERT(invariant());
        unsigned_vector bound_trail, bound_vars;
        TRACE("opt", display(tout << "tableau\n"););
        while (!objective().m_vars.empty()) {
            var v = objective().m_vars.back();
            unsigned x = v.m_id;
            rational const& coeff = v.m_coeff;
            unsigned bound_row_index;
            rational bound_coeff;
            if (find_bound(x, bound_row_index, bound_coeff, coeff.is_pos())) {
                SASSERT(!bound_coeff.is_zero());
                TRACE("opt", display(tout << "update: " << v << " ", objective());
                      for (unsigned i = 0; i < m_above.size(); ++i) {
                          display(tout << "resolve: ", m_rows[m_above[i]]);
                      });
                for (unsigned i = 0; i < m_above.size(); ++i) {
                    resolve(bound_row_index, bound_coeff, m_above[i], x);
                }
                for (unsigned i = 0; i < m_below.size(); ++i) {
                    resolve(bound_row_index, bound_coeff, m_below[i], x);
                }
                // coeff*x + objective <= ub
                // a2*x + t2 <= 0
                // => coeff*x <= -t2*coeff/a2
                // objective + t2*coeff/a2 <= ub

                mul_add(false, m_objective_id, - coeff/bound_coeff, bound_row_index);
                m_rows[bound_row_index].m_alive = false;
                bound_trail.push_back(bound_row_index);
                bound_vars.push_back(x);
            }
            else {
                TRACE("opt", display(tout << "unbound: " << v << " ", objective()););
                update_values(bound_vars, bound_trail);
                return inf_eps::infinity();
            }
        }

        //
        // update the evaluation of variables to satisfy the bound.
        //

        update_values(bound_vars, bound_trail);

        rational value = objective().m_value;
        if (objective().m_type == t_lt) {            
            return inf_eps(inf_rational(value, rational(-1)));
        }
        else {
            return inf_eps(inf_rational(value));
        }
    }


    void model_based_opt::update_value(unsigned x, rational const& val) {
        rational old_val = m_var2value[x];
        m_var2value[x] = val;
        unsigned_vector const& row_ids = m_var2row_ids[x];
        for (unsigned i = 0; i < row_ids.size(); ++i) {            
            unsigned row_id = row_ids[i];
            rational coeff = get_coefficient(row_id, x);
            if (coeff.is_zero()) {
                continue;
            }
            row & r = m_rows[row_id];
            rational delta = coeff * (val - old_val);            
            r.m_value += delta;
            SASSERT(invariant(row_id, r));
        }
    }


    void model_based_opt::update_values(unsigned_vector const& bound_vars, unsigned_vector const& bound_trail) {
        for (unsigned i = bound_trail.size(); i > 0; ) {
            --i;
            unsigned x = bound_vars[i];
            row& r = m_rows[bound_trail[i]];
            rational val = r.m_coeff;
            rational old_x_val = m_var2value[x];
            rational new_x_val;
            rational x_coeff, eps(0);
            vector<var> const& vars = r.m_vars;
            for (unsigned j = 0; j < vars.size(); ++j) {
                var const& v = vars[j];
                if (x == v.m_id) {
                    x_coeff = v.m_coeff;
                }
                else {
                    val += m_var2value[v.m_id]*v.m_coeff;
                }
            }
            SASSERT(!x_coeff.is_zero());
            new_x_val = -val/x_coeff;

            if (r.m_type == t_lt) {
                eps = abs(old_x_val - new_x_val)/rational(2);
                eps = std::min(rational::one(), eps);
                SASSERT(!eps.is_zero());

                //
                //     ax + t < 0
                // <=> x < -t/a
                // <=> x := -t/a - epsilon
                // 
                if (x_coeff.is_pos()) {
                    new_x_val -= eps;
                }
                //
                //     -ax + t < 0 
                // <=> -ax < -t
                // <=> -x < -t/a
                // <=> x > t/a
                // <=> x := t/a + epsilon
                //
                else {
                    new_x_val += eps;
                }
            }
            TRACE("opt", display(tout << "v" << x 
                                 << " coeff_x: " << x_coeff 
                                 << " old_x_val: " << old_x_val
                                 << " new_x_val: " << new_x_val
                                 << " eps: " << eps << " ", r); );
            m_var2value[x] = new_x_val;
            
            r.m_value = get_row_value(r);
            SASSERT(invariant(bound_trail[i], r));
        }        
        
        // update and check bounds for all other affected rows.
        for (unsigned i = bound_trail.size(); i > 0; ) {
            --i;
            unsigned x = bound_vars[i];
            unsigned_vector const& row_ids = m_var2row_ids[x];
            for (unsigned j = 0; j < row_ids.size(); ++j) {            
                unsigned row_id = row_ids[j];
                row & r = m_rows[row_id];
                r.m_value = get_row_value(r);
                SASSERT(invariant(row_id, r));
            }            
        }
        SASSERT(invariant());
    }

    bool model_based_opt::find_bound(unsigned x, unsigned& bound_row_index, rational& bound_coeff, bool is_pos) {
        bound_row_index = UINT_MAX;
        rational lub_val;
        rational const& x_val = m_var2value[x];
        unsigned_vector const& row_ids = m_var2row_ids[x];
        uint_set visited;
        m_above.reset();
        m_below.reset();
        for (unsigned i = 0; i < row_ids.size(); ++i) {
            unsigned row_id = row_ids[i];
            SASSERT(row_id != m_objective_id);
            if (visited.contains(row_id)) {
                continue;
            }
            visited.insert(row_id);
            row& r = m_rows[row_id];
            if (r.m_alive) {
                rational a = get_coefficient(row_id, x);
                if (a.is_zero()) {
                    // skip
                }
                else if (a.is_pos() == is_pos || r.m_type == t_eq) {
                    rational value = x_val - (r.m_value/a);
                    if (bound_row_index == UINT_MAX) {
                        lub_val = value;
                        bound_row_index = row_id;
                        bound_coeff = a;
                    }
                    else if ((value == lub_val && r.m_type == opt::t_lt) ||
                             (is_pos && value < lub_val) || 
                             (!is_pos && value > lub_val)) {
                        m_above.push_back(bound_row_index);
                        lub_val = value;
                        bound_row_index = row_id;                          
                        bound_coeff = a;
                    }
                    else {
                        m_above.push_back(row_id);
                    }
                }
                else {
                    m_below.push_back(row_id);
                }
            }
        }
        return bound_row_index != UINT_MAX;
    }
       
    rational model_based_opt::get_row_value(row const& r) const {
        vector<var> const& vars = r.m_vars;
        rational val = r.m_coeff;
        for (unsigned i = 0; i < vars.size(); ++i) {
            var const& v = vars[i];
            val += v.m_coeff * m_var2value[v.m_id];
        }
        return val;
    }    
 
    rational model_based_opt::get_coefficient(unsigned row_id, unsigned var_id) const {
        row const& r = m_rows[row_id];
        if (r.m_vars.empty()) {
            return rational::zero();
        }
        unsigned lo = 0, hi = r.m_vars.size();
        while (lo < hi) {
            unsigned mid = lo + (hi - lo)/2;
            SASSERT(mid < hi);
            unsigned id = r.m_vars[mid].m_id;
            if (id == var_id) {
                lo = mid;
                break;
            }
            if (id < var_id) {
                lo = mid + 1;
            }			
            else {
                hi = mid;
            }
        }
        if (lo == r.m_vars.size()) {
            return rational::zero();
        }
        unsigned id = r.m_vars[lo].m_id;
        if (id == var_id) {
            return r.m_vars[lo].m_coeff;
        }
        else {
            return rational::zero();
        }
    }

    // 
    // Let
    //   row1: t1 + a1*x <= 0
    //   row2: t2 + a2*x <= 0
    //
    // assume a1, a2 have the same signs:
    //       (t2 + a2*x) <= (t1 + a1*x)*a2/a1 
    //   <=> t2*a1/a2 - t1 <= 0
    //   <=> t2 - t1*a2/a1 <= 0
    //
    // assume a1 > 0, -a2 < 0:
    //       t1 + a1*x <= 0,  t2 - a2*x <= 0
    //       t2/a2 <= -t1/a1
    //       t2 + t1*a2/a1 <= 0
    // assume -a1 < 0, a2 > 0:
    //       t1 - a1*x <= 0,  t2 + a2*x <= 0
    //       t1/a1 <= -t2/a2
    //       t2 + t1*a2/a1 <= 0
    // 
    // the resolvent is the same in all cases (simpler proof should exist)
    // 

    void model_based_opt::resolve(unsigned row_src, rational const& a1, unsigned row_dst, unsigned x) {

        SASSERT(a1 == get_coefficient(row_src, x));
        SASSERT(!a1.is_zero());
        SASSERT(row_src != row_dst);
                
        if (m_rows[row_dst].m_alive) {
            rational a2 = get_coefficient(row_dst, x);
            mul_add(row_dst != m_objective_id && a1.is_pos() == a2.is_pos(), row_dst, -a2/a1, row_src);            
        }
    }
    
    //
    // set row1 <- row1 + c*row2
    //
    void model_based_opt::mul_add(bool same_sign, unsigned row_id1, rational const& c, unsigned row_id2) {
        if (c.is_zero()) {
            return;
        }
        m_new_vars.reset();
        row& r1 = m_rows[row_id1];
        row const& r2 = m_rows[row_id2];
        unsigned i = 0, j = 0;
        for(; i < r1.m_vars.size() || j < r2.m_vars.size(); ) {
            if (j == r2.m_vars.size()) {
                m_new_vars.append(r1.m_vars.size() - i, r1.m_vars.c_ptr() + i);
                break;
            }
            if (i == r1.m_vars.size()) {
                for (; j < r2.m_vars.size(); ++j) {
                    m_new_vars.push_back(r2.m_vars[j]);
                    m_new_vars.back().m_coeff *= c;
                    if (row_id1 != m_objective_id) {
                        m_var2row_ids[r2.m_vars[j].m_id].push_back(row_id1);
                    }
                }
                break;
            }

            unsigned v1 = r1.m_vars[i].m_id;
            unsigned v2 = r2.m_vars[j].m_id;
            if (v1 == v2) {
                m_new_vars.push_back(r1.m_vars[i]);
                m_new_vars.back().m_coeff += c*r2.m_vars[j].m_coeff;
                ++i;
                ++j;
                if (m_new_vars.back().m_coeff.is_zero()) {
                    m_new_vars.pop_back();
                }
            }
            else if (v1 < v2) {
                m_new_vars.push_back(r1.m_vars[i]);
                ++i;                        
            }
            else {
                m_new_vars.push_back(r2.m_vars[j]);
                m_new_vars.back().m_coeff *= c;
                if (row_id1 != m_objective_id) {
                    m_var2row_ids[r2.m_vars[j].m_id].push_back(row_id1);
                }
                ++j;                        
            }
        }
        r1.m_coeff += c*r2.m_coeff;
        r1.m_vars.swap(m_new_vars);
        r1.m_value += c*r2.m_value;

        if (!same_sign && r2.m_type == t_lt) {
            r1.m_type = t_lt;
        }
        else if (same_sign && r1.m_type == t_lt && r2.m_type == t_lt) {
            r1.m_type = t_le;        
        }
        SASSERT(invariant(row_id1, r1));
    }
    
    void model_based_opt::display(std::ostream& out) const {
        for (unsigned i = 0; i < m_rows.size(); ++i) {
            display(out, m_rows[i]);
        }
        for (unsigned i = 0; i < m_var2row_ids.size(); ++i) {
            unsigned_vector const& rows = m_var2row_ids[i];
            out << i << ": ";
            for (unsigned j = 0; j < rows.size(); ++j) {
                out << rows[j] << " ";
            }
            out << "\n";
        }
    }        

    void model_based_opt::display(std::ostream& out, row const& r) const {
        vector<var> const& vars = r.m_vars;
        out << (r.m_alive?"+":"-") << " ";
        for (unsigned i = 0; i < vars.size(); ++i) {
            if (i > 0 && vars[i].m_coeff.is_pos()) {
                out << "+ ";
            }
            out << vars[i].m_coeff << "* v" << vars[i].m_id << " ";                
        }
        if (r.m_coeff.is_pos()) {
            out << " + " << r.m_coeff << " ";
        }
        else if (r.m_coeff.is_neg()) {
            out << r.m_coeff << " ";
        }        
        out << r.m_type << " 0; value: " << r.m_value  << "\n";
    }

    unsigned model_based_opt::add_var(rational const& value) {
        unsigned v = m_var2value.size();
        m_var2value.push_back(value);
        m_var2row_ids.push_back(unsigned_vector());
        return v;
    }

    rational model_based_opt::get_value(unsigned var) {
        return m_var2value[var];
    }

    void model_based_opt::set_row(unsigned row_id, vector<var> const& coeffs, rational const& c, ineq_type rel) {
        row& r = m_rows[row_id];
        rational val(c);
        SASSERT(r.m_vars.empty());
        r.m_vars.append(coeffs.size(), coeffs.c_ptr());
        std::sort(r.m_vars.begin(), r.m_vars.end(), var::compare());
        for (unsigned i = 0; i < coeffs.size(); ++i) {
            val += m_var2value[coeffs[i].m_id] * coeffs[i].m_coeff;
        }
        r.m_alive = true;
        r.m_coeff = c;
        r.m_value = val;
        r.m_type = rel;
        SASSERT(invariant(row_id, r));
    }

    void model_based_opt::add_constraint(vector<var> const& coeffs, rational const& c, ineq_type rel) {
        rational val(c);        
        unsigned row_id = m_rows.size();
        m_rows.push_back(row());
        set_row(row_id, coeffs, c, rel);
        for (unsigned i = 0; i < coeffs.size(); ++i) {
            m_var2row_ids[coeffs[i].m_id].push_back(row_id); 
        }
    }

    void model_based_opt::set_objective(vector<var> const& coeffs, rational const& c) {
        set_row(m_objective_id, coeffs, c, t_le);
    }

    void model_based_opt::get_live_rows(vector<row>& rows) {
        for (unsigned i = 0; i < m_rows.size(); ++i) {
            if (m_rows[i].m_alive) {
                rows.push_back(m_rows[i]);
            }
        }
    }

    //
    // pick glb and lub representative.
    // The representative is picked such that it 
    // represents the fewest inequalities. 
    // The constraints that enforce a glb or lub are not forced.
    // The constraints that separate the glb from ub or the lub from lb
    // are not forced.
    // In other words, suppose there are 
    // . N inequalities of the form t <= x
    // . M inequalities of the form s >= x
    // . t0 is glb among N under valuation.
    // . s0 is lub among M under valuation.
    // If N < M
    //    create the inequalities:
    //       t <= t0 for each t other than t0 (N-1 inequalities).
    //       t0 <= s for each s (M inequalities).
    // If N >= M the construction is symmetric.
    // 
    void model_based_opt::project(unsigned x) {
        unsigned_vector& lub_rows = m_lub;
        unsigned_vector& glb_rows = m_glb;        
        unsigned lub_index = UINT_MAX, glb_index = UINT_MAX;
        bool     lub_strict = false, glb_strict = false;
        rational lub_val, glb_val;
        rational const& x_val = m_var2value[x];
        unsigned_vector const& row_ids = m_var2row_ids[x];
        uint_set visited;
        lub_rows.reset();
        glb_rows.reset();
        // select the lub and glb.
        for (unsigned i = 0; i < row_ids.size(); ++i) {
            unsigned row_id = row_ids[i];
            if (visited.contains(row_id)) {
                continue;
            }
            visited.insert(row_id);
            row& r = m_rows[row_id];
            if (!r.m_alive) {
                continue;
            }
            rational a = get_coefficient(row_id, x);
            if (a.is_zero()) {
                continue;
            }
            if (r.m_type == t_eq) {
                solve_for(row_id, x);
                return;
            }
            if (a.is_pos()) {
                rational lub_value = x_val - (r.m_value/a);
                if (lub_rows.empty() || 
                    lub_value < lub_val ||
                    (lub_value == lub_val && r.m_type == t_lt && !lub_strict)) {
                    lub_val = lub_value;
                    lub_index = row_id;
                    lub_strict = r.m_type == t_lt;                    
                }
                lub_rows.push_back(row_id);
            }
            else {
                SASSERT(a.is_neg());
                rational glb_value = x_val - (r.m_value/a);
                if (glb_rows.empty() || 
                    glb_value > glb_val ||
                    (glb_value == glb_val && r.m_type == t_lt && !glb_strict)) {
                    glb_val = glb_value;
                    glb_index = row_id;
                    glb_strict = r.m_type == t_lt;                    
                }
                glb_rows.push_back(row_id);
            }
        }
        unsigned row_index = (lub_rows.size() <= glb_rows.size())? lub_index : glb_index;
        glb_rows.append(lub_rows);            
        if (row_index == UINT_MAX) {
            for (unsigned i = 0; i < glb_rows.size(); ++i) {
                unsigned row_id = glb_rows[i];
                SASSERT(m_rows[row_id].m_alive);
                SASSERT(!get_coefficient(row_id, x).is_zero());
                m_rows[row_id].m_alive = false;
            }
        }
        else {
            rational coeff = get_coefficient(row_index, x);
            for (unsigned i = 0; i < glb_rows.size(); ++i) {
                unsigned row_id = glb_rows[i];
                if (row_id != row_index) {
                    resolve(row_index, coeff, row_id, x);
                }
            }
            m_rows[row_index].m_alive = false;
        }
    }

    void model_based_opt::solve_for(unsigned row_id1, unsigned x) {
        rational a = get_coefficient(row_id1, x);
        row& r1 = m_rows[row_id1];
        SASSERT(!a.is_zero());
        SASSERT(r1.m_type == t_eq);
        SASSERT(r1.m_alive);
        unsigned_vector const& row_ids = m_var2row_ids[x];
        uint_set visited;
        visited.insert(row_id1);
        for (unsigned i = 0; i < row_ids.size(); ++i) {
            unsigned row_id2 = row_ids[i];
            if (!visited.contains(row_id2)) {                
                visited.insert(row_id2);
                resolve(row_id1, a, row_id2, x);            
            }
        }
        r1.m_alive = false;
    }

    void model_based_opt::project(unsigned num_vars, unsigned const* vars) {
        for (unsigned i = 0; i < num_vars; ++i) {
            project(vars[i]);
        }
    }

}

