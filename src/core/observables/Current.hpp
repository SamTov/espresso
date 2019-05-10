/*
Copyright (C) 2010-2018 The ESPResSo project

This file is part of ESPResSo.

ESPResSo is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

ESPResSo is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef OBSERVABLES_CURRENTS_HPP
#define OBSERVABLES_CURRENTS_HPP

#include "PidObservable.hpp"

#include <vector>

namespace Observables {

class Current : public PidObservable {
public:
  using PidObservable::PidObservable;
  int n_values() const override { return 3; };
  std::vector<double> operator()(PartCfg &partCfg) const override {
    std::vector<double> res(n_values());
    for (int i : ids()) {
#ifdef ELECTROSTATICS
      double charge = partCfg[i].p.q;
      res[0] += charge * partCfg[i].m.v[0];
      res[1] += charge * partCfg[i].m.v[1];
      res[2] += charge * partCfg[i].m.v[2];
#endif
    };
    return res;
  };
};

} // Namespace Observables
#endif
