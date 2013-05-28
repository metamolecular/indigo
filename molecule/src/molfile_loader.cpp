/****************************************************************************
 * Copyright (C) 2009-2013 GGA Software Services LLC
 * 
 * This file is part of Indigo toolkit.
 * 
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 3 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 * 
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 ***************************************************************************/

#include "base_cpp/scanner.h"
#include "base_cpp/tlscont.h"
#include "base_cpp/auto_ptr.h"

#include "molecule/molfile_loader.h"
#include "molecule/molecule.h"
#include "molecule/query_molecule.h"
#include "molecule/molecule_stereocenters.h"
#include "molecule/molecule_3d_constraints.h"
#include "molecule/elements.h"
#include "molecule/smiles_loader.h"

#define STRCMP(a, b) strncmp((a), (b), strlen(b))

using namespace indigo;

IMPL_ERROR(MolfileLoader, "molfile loader");

MolfileLoader::MolfileLoader (Scanner &scanner) : 
_scanner(scanner),
TL_CP_GET(_stereo_care_atoms),
TL_CP_GET(_stereo_care_bonds),
TL_CP_GET(_stereocenter_types),
TL_CP_GET(_stereocenter_groups),
TL_CP_GET(_sensible_bond_directions),
TL_CP_GET(_ignore_cistrans),
TL_CP_GET(_atom_types),
TL_CP_GET(_hcount),
TL_CP_GET(_sgroup_types),
TL_CP_GET(_sgroup_mapping)
{
   reaction_atom_mapping = 0;
   reaction_atom_inversion = 0;
   reaction_atom_exact_change = 0;
   reaction_bond_reacting_center = 0;
   _rgfile = false;
   ignore_stereocenter_errors = false;
   treat_x_as_pseudoatom = false;
   skip_3d_chirality = false;
   ignore_noncritical_query_features = false;
}

void MolfileLoader::loadMolecule (Molecule &mol)
{
   mol.clear();
   _bmol = &mol;
   _mol = &mol;
   _qmol = 0;
   _loadMolecule();

   if (mol.stereocenters.size() == 0 && !skip_3d_chirality)
      mol.stereocenters.buildFrom3dCoordinates();
}

void MolfileLoader::loadQueryMolecule (QueryMolecule &mol)
{
   mol.clear();
   _bmol = &mol;
   _qmol = &mol;
   _mol = 0;
   _loadMolecule();

   if (mol.stereocenters.size() == 0)
      mol.stereocenters.buildFrom3dCoordinates();
}

void MolfileLoader::_loadMolecule ()
{
   _readHeader();

   _readCtabHeader();

   if (_v2000)
   {
      _readCtab2000();

      if (_rgfile)
         _readRGroups2000();
   }
   else
   {
      _readCtab3000();
      _readRGroups3000();
   }

   _postLoad();
}

void MolfileLoader::loadCtab3000 (Molecule &mol)
{
   _bmol = &mol;
   _qmol = 0;
   _mol = &mol;
   _readCtab3000();
   _postLoad();
}

void MolfileLoader::loadQueryCtab3000 (QueryMolecule &mol)
{
   _bmol = &mol;
   _qmol = &mol;
   _mol = 0;
   _readCtab3000();
   _postLoad();
}

void MolfileLoader::_readHeader ()
{
   if (_scanner.lookNext() == '$')
   {
      _rgfile = true;      // It's RGfile
      _scanner.skipLine(); // Skip $MDL REV  1   Date/Time
      _scanner.skipLine(); // Skip $MOL
      _scanner.skipLine(); // Skip $HDR
   }

   // Skip header
   _scanner.readLine(_bmol->name, true);
   _scanner.skipLine();
   _scanner.skipLine();

   if (_rgfile)
   {
      _scanner.skipLine(); // Skip $END HDR
      _scanner.skipLine(); // Skip $CTAB
   }
}

void MolfileLoader::_readCtabHeader ()
{
   QS_DEF(Array<char>, str);

   _scanner.readLine(str, false);

   BufferScanner strscan(str);

   _atoms_num = strscan.readIntFix(3);
   _bonds_num = strscan.readIntFix(3);

   try
   {
      char version[6];
      int chiral_int;
      
      strscan.skip(6);
      chiral_int = strscan.readIntFix(3);
      strscan.skip(19);
      strscan.read(5, version);
      strscan.skipLine();

      version[5] = 0;

      if (strcasecmp(version, "V2000") == 0)
         _v2000 = true;
      else if (strcasecmp(version, "V3000") == 0)
         _v2000 = false;
      else
         throw Error("bad molfile version : %s", version);

      _chiral = (chiral_int != 0);
   }
   catch (Scanner::Error &)
   {
      _chiral = false;
      _v2000 = true;
   }
}

int MolfileLoader::_getElement (const char *buf)
{
   char buf2[4] = {0, 0, 0, 0};

   size_t len = strlen(buf);
   if (len > 3)
      throw Error("Internal error in MolfileLoader::_getElement: len = %d > 3", len);

   for (size_t i = 0; i < len; i++)
   {
      if (isspace(buf[i]))
         break;

      if (!isalpha(buf[i]))
         return -1;

      buf2[i] = (i == 0) ? toupper(buf[i]) : tolower(buf[i]);
   }

   return Element::fromString2(buf2);
}

char* MolfileLoader::_strtrim (char *buf)
{
   while (*buf == ' ')
      buf++;
   if (*buf != 0)
   {
      size_t len = strlen(buf);
      char *end = buf + len - 1;
      while (*end == ' ')
      {
         *end = 0;
         end--;
      }
   }
   return buf;
}

void MolfileLoader::_readCtab2000 ()
{
   _init();

   int k;

   QS_DEF(Array<char>, str);

   // read atoms
   for (k = 0; k < _atoms_num; k++)
   {
      // read coordinates
      float x = _scanner.readFloatFix(10);
      float y = _scanner.readFloatFix(10);
      float z = _scanner.readFloatFix(10);

      _scanner.skip(1);

      char atom_label_array[4] = { 0 };
      char *buf = atom_label_array;
      int label = 0;
      int isotope = 0;

      int &atom_type = _atom_types.push();

      _hcount.push(0);

      atom_type = _ATOM_ELEMENT;

      // read atom label and mass difference
      _scanner.readCharsFix(3, atom_label_array);

      // Atom label can be both left-bound or right-bound: "  N", "N  " or even " N ".
      buf = _strtrim(atom_label_array);

      isotope = _scanner.readIntFix(2);

      if (buf[0] == 0)
         throw Error("Empty atom label");
      else if (buf[0] == 'R' && (buf[1] == '#' || buf[1] == 0))
      {
         atom_type = _ATOM_R;
         label = ELEM_RSITE;
      }
      else if (buf[0] == 'A' && buf[1] == 0)
         atom_type = _ATOM_A; // will later become 'any atom' or pseudo atom
      else if (buf[0] == 'X' && buf[1] == 0 && !treat_x_as_pseudoatom)
      {
         if (_qmol == 0)
            throw Error("'X' label is allowed only for queries");
         atom_type = _ATOM_X;
      }
      else if (buf[0] == 'Q' && buf[1] == 0)
      {
         if (_qmol == 0)
            throw Error("'Q' label is allowed only for queries");
         atom_type = _ATOM_Q;
      }
      else if (buf[0] == 'L' && buf[1] == 0)
      {
         if (_qmol == 0)
            throw Error("atom lists are allowed only for queries");
         atom_type = _ATOM_LIST;
      }
      else if (buf[0] == 'D' && buf[1] == 0)
      {
         label = ELEM_H;
         isotope = 2;
      }
      else if (buf[0] == 'T' && buf[1] == 0)
      {
         label = ELEM_H;
         isotope = 3;
      }
      else
      {
         label = _getElement(buf);

         if (label == -1)
         {
            atom_type = _ATOM_PSEUDO;
            if (isotope != 0)
               throw Error("isotope number not allowed on pseudo-atoms");
         }

         if (isotope != 0)
            isotope = Element::getDefaultIsotope(label) + isotope;
      }

      int stereo_care = 0, valence = 0;
      int aam = 0, irflag = 0, ecflag = 0;
      int charge = 0, radical = 0;

      _convertCharge(_scanner.readIntFix(3), charge, radical);

      try
      {
         _scanner.readLine(str, false);

         BufferScanner rest(str);

         rest.skip(3); // skip atom stereo parity
         _hcount[k] = rest.readIntFix(3);
         stereo_care = rest.readIntFix(3);

         if (stereo_care > 0 && _qmol == 0)
            if (!ignore_noncritical_query_features)
               throw Error("only a query can have stereo care box");

         valence = rest.readIntFix(3);
         rest.skip(9); // skip "HO designator" and 2 unused fields
         aam = rest.readIntFix(3);    // atom-to-atom mapping number
         irflag = rest.readIntFix(3); // inversion/retension flag,
         ecflag = rest.readIntFix(3); // exact change flag

      }
      catch (Scanner::Error &)
      {
      }

      int idx;

      if (_mol != 0)
      {
         idx = _mol->addAtom(label);

         if (atom_type == _ATOM_PSEUDO)
            _mol->setPseudoAtom(idx, buf);

         _mol->setAtomCharge_Silent(idx, charge);
         _mol->setAtomIsotope(idx, isotope);
         _mol->setAtomRadical(idx, radical);

         if (valence > 0 && valence <= 14)
            _mol->setExplicitValence(idx, valence);
         if (valence == 15)
            _mol->setExplicitValence(idx, 0);
         
         _bmol->setAtomXyz(idx, x, y, z);
      }
      else
      {
         AutoPtr<QueryMolecule::Atom> atom;

         if (atom_type == _ATOM_ELEMENT)
            atom.reset(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, label));
         else if (atom_type == _ATOM_PSEUDO)
            atom.reset(new QueryMolecule::Atom(QueryMolecule::ATOM_PSEUDO, buf));
         else if (atom_type == _ATOM_A)
            atom.reset(QueryMolecule::Atom::nicht(
                        new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_H)));
         else if (atom_type == _ATOM_Q)
            atom.reset(QueryMolecule::Atom::und(
                           QueryMolecule::Atom::nicht(
                              new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_H)),
                           QueryMolecule::Atom::nicht(
                              new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_C))));
         else if (atom_type == _ATOM_X)
         {
            atom.reset(new QueryMolecule::Atom());

            atom->type = QueryMolecule::OP_OR;
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_F));
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_Cl));
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_Br));
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_I));
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_At));
         }
         else if (atom_type == _ATOM_R)
            atom.reset(new QueryMolecule::Atom(QueryMolecule::ATOM_RSITE, 0));
         else // _ATOM_LIST
            atom.reset(new QueryMolecule::Atom());
         
         if (charge != 0)
            atom.reset(QueryMolecule::Atom::und(atom.release(),
               new QueryMolecule::Atom(QueryMolecule::ATOM_CHARGE, charge)));
         if (isotope != 0)
            atom.reset(QueryMolecule::Atom::und(atom.release(),
               new QueryMolecule::Atom(QueryMolecule::ATOM_ISOTOPE, isotope)));
         if (radical != 0)
            atom.reset(QueryMolecule::Atom::und(atom.release(),
               new QueryMolecule::Atom(QueryMolecule::ATOM_RADICAL, radical)));
         if (valence > 0)
         {
            if (valence == 15)
               valence = 0;
            atom.reset(QueryMolecule::Atom::und(atom.release(),
               new QueryMolecule::Atom(QueryMolecule::ATOM_VALENCE, valence)));
         }

         idx = _qmol->addAtom(atom.release());
         _bmol->setAtomXyz(idx, x, y, z);
      }

      if (stereo_care)
         _stereo_care_atoms[idx] = 1;

      if (reaction_atom_mapping != 0)
         reaction_atom_mapping->at(idx) = aam;
      if (reaction_atom_inversion != 0)
         reaction_atom_inversion->at(idx) = irflag;
      if (reaction_atom_exact_change != 0)
         reaction_atom_exact_change->at(idx) = ecflag;
   }

   int bond_idx;

   for (bond_idx = 0; bond_idx < _bonds_num; bond_idx++)
   {
      int beg = _scanner.readIntFix(3);
      int end = _scanner.readIntFix(3);
      int order = _scanner.readIntFix(3);
      int stereo = _scanner.readIntFix(3);
      int topology = 0;

      try
      {
         _scanner.readLine(str, false);

         BufferScanner rest(str);

         rest.skip(3); // not used

         topology = rest.readIntFix(3);

         if (topology != 0 && _qmol == 0)
            if (!ignore_noncritical_query_features)
               throw Error("bond topology is allowed only for queries");

         int rcenter = rest.readIntFix(3);

         if (reaction_bond_reacting_center != 0)
            reaction_bond_reacting_center->at(bond_idx) = rcenter;
      }
      catch (Scanner::Error &)
      {
      }

      if (_mol != 0)
      {
         if (order == BOND_SINGLE || order == BOND_DOUBLE ||
             order == BOND_TRIPLE || order == BOND_AROMATIC)
            _mol->addBond_Silent(beg - 1, end - 1, order);
         else if (order == _BOND_SINGLE_OR_DOUBLE)
            throw Error("'single or double' bonds are allowed only for queries");
         else if (order == _BOND_SINGLE_OR_AROMATIC)
            throw Error("'single or aromatic' bonds are allowed only for queries");
         else if (order == _BOND_DOUBLE_OR_AROMATIC)
            throw Error("'double or aromatic' bonds are allowed only for queries");
         else if (order == _BOND_ANY)
            throw Error("'any' bonds are allowed only for queries");
         else
            throw Error("unknown bond type: %d", order);
      }
      else
      {
         AutoPtr<QueryMolecule::Bond> bond;

         if (order == BOND_SINGLE || order == BOND_DOUBLE ||
             order == BOND_TRIPLE || order == BOND_AROMATIC)
            bond.reset(new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, order));
         else if (order == _BOND_SINGLE_OR_DOUBLE)
            bond.reset(QueryMolecule::Bond::und(
               QueryMolecule::Bond::nicht(
                 new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_AROMATIC)),
               QueryMolecule::Bond::oder(
                 new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_SINGLE),
                 new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_DOUBLE))));
         else if (order == _BOND_SINGLE_OR_AROMATIC)
            bond.reset(QueryMolecule::Bond::oder(
              new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_SINGLE),
              new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_AROMATIC)));
         else if (order == _BOND_DOUBLE_OR_AROMATIC)
            bond.reset(QueryMolecule::Bond::oder(
              new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_DOUBLE),
              new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_AROMATIC)));
         else if (order == _BOND_ANY)
            bond.reset(new QueryMolecule::Bond());
         else
            throw Error("unknown bond type: %d", order);

         if (topology != 0)
         {
            bond.reset(QueryMolecule::Bond::und(bond.release(),
               new QueryMolecule::Bond(QueryMolecule::BOND_TOPOLOGY,
                 topology == 1 ? TOPOLOGY_RING : TOPOLOGY_CHAIN)));
         }

         _qmol->addBond(beg - 1, end - 1, bond.release());
      }

      if (stereo == 1)
         _bmol->setBondDirection(bond_idx, BOND_UP);
      else if (stereo == 6)
         _bmol->setBondDirection(bond_idx, BOND_DOWN);
      else if (stereo == 4)
         _bmol->setBondDirection(bond_idx, BOND_EITHER);
      else if (stereo == 3)
         _ignore_cistrans[bond_idx] = 1;
      else if (stereo != 0)
         throw Error("unknown number for bond stereo: %d", stereo);
   }

   int n_3d_features = -1;

   // read groups
   while (!_scanner.isEOF())
   {
      char c = _scanner.readChar();

      if (c == 'G')
      {
         _scanner.skipLine();
         _scanner.skipLine();
         continue;
      }
      if (c == 'M')
      {
         _scanner.skip(2);
         char chars[4] = {0, 0, 0, 0};

         _scanner.readCharsFix(3, chars);

         if (strncmp(chars, "END", 3) == 0)
         {
            _scanner.skipLine();
            break;
         }
         // atom list
         else if (strncmp(chars, "ALS", 3) == 0)
         {
            if (_qmol == 0)
               throw Error("atom lists are allowed only for queries");

            int i;

            _scanner.skip(1);
            int atom_idx = _scanner.readIntFix(3);
            int list_size = _scanner.readIntFix(3);
            _scanner.skip(1);
            char excl_char = _scanner.readChar();
            _scanner.skip(1);

            atom_idx--;

            AutoPtr<QueryMolecule::Atom> atomlist;

            _scanner.readLine(str, false);
            BufferScanner rest(str);

            for (i = 0; i < list_size; i++)
            {
               int j;

               memset(chars, 0, sizeof(chars));

               for (j = 0; j < 4; j++)
               {
                  // can not read 4 characters at once because
                  // sqlplus cuts the trailing spaces
                  if (!rest.isEOF())
                     chars[j] = rest.readChar();
                  else
                     break;
               }

               if (j < 1)
                  throw Error("atom list: can not read element #%d", i);

               for (j = 0; j < 4; j++)
                  if (chars[j] == ' ')
                     memset(chars + j, 0, 4 - j);

               if (chars[3] != 0)
                  throw Error("atom list: invalid element '%c%c%c%c'",
                               chars[0], chars[1], chars[2], chars[3]);

               if (chars[0] == 'A' && chars[1] == 0)
               {
                  if (list_size == 1)
                     atomlist.reset(QueryMolecule::Atom::nicht(
                              new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_H)));
                  else
                     throw Error("'A' inside atom list, if present, must be single");
               }
               else if (chars[0] == 'Q' && chars[1] == 0)
               {
                  if (list_size == 1)
                     atomlist.reset(QueryMolecule::Atom::und(
                           QueryMolecule::Atom::nicht(
                              new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_H)),
                           QueryMolecule::Atom::nicht(
                              new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_C))));
                  else
                     throw Error("'Q' inside atom list, if present, must be single");
               }
               else
               {
                  _appendQueryAtom(chars, atomlist);
               }
            }

            if (excl_char == 'T')
               atomlist.reset(QueryMolecule::Atom::nicht(atomlist.release()));

            _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
               atomlist.release()));

         }
         // atom charge
         else if (strncmp(chars, "CHG", 3) == 0)
         {
            int n = _scanner.readIntFix(3);

            while (n-- > 0)
            {
               _scanner.skip(1);
               int atom_idx = _scanner.readIntFix(3) - 1;
               _scanner.skip(1);
               int charge = _scanner.readIntFix(3);

               if (_mol != 0)
                  _mol->setAtomCharge_Silent(atom_idx, charge);
               else
               {
                  _qmol->getAtom(atom_idx).removeConstraints(QueryMolecule::ATOM_CHARGE);
                  _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_CHARGE, charge)));
               }
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "RAD", 3) == 0)
         {
            int n = _scanner.readIntFix(3);

            while (n-- > 0)
            {
               _scanner.skip(1);
               int atom_idx = _scanner.readIntFix(3) - 1;
               _scanner.skip(1);
               int radical = _scanner.readIntFix(3);

               if (_mol != 0)
                  _mol->setAtomRadical(atom_idx, radical);
               else
               {
                  _qmol->getAtom(atom_idx).removeConstraints(QueryMolecule::ATOM_RADICAL);
                  _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_RADICAL, radical)));
               }

            }
            _scanner.skipLine();
         }
         // atom isotope
         else if (strncmp(chars, "ISO", 3) == 0)
         {
            int n = _scanner.readIntFix(3);

            while (--n >= 0)
            {
               _scanner.skip(1);
               int atom_idx = _scanner.readIntFix(3) - 1;
               _scanner.skip(1);
               int isotope = _scanner.readIntFix(3);

               if (_mol != 0)
                  _mol->setAtomIsotope(atom_idx, isotope);
               else
               {
                  _qmol->getAtom(atom_idx).removeConstraints(QueryMolecule::ATOM_ISOTOPE);
                  _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_ISOTOPE, isotope)));
               }
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "SUB", 3) == 0)
         {
            if (_qmol == 0)
               throw Error("substitution counts are allowed only for queries");

            int n = _scanner.readIntFix(3);

            while (n-- > 0)
            {
               _scanner.skip(1);
               int atom_idx = _scanner.readIntFix(3) - 1;
               _scanner.skip(1);
               int sub_count = _scanner.readIntFix(3);

               if (sub_count == -1) // no substitution
                  _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_SUBSTITUENTS, 0)));
               else if (sub_count == -2)
               {
                  _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_SUBSTITUENTS_AS_DRAWN, _qmol->getVertex(atom_idx).degree())));
               }
               else if (sub_count > 0)
                  _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_SUBSTITUENTS,
                          sub_count, (sub_count < 6 ? sub_count : 100))));
               else
                  throw Error("invalid SUB value: %d", sub_count);
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "RBC", 3) == 0)
         {
            if (_qmol == 0)
            {
               if (!ignore_noncritical_query_features)
                  throw Error("ring bond count is allowed only for queries");
            }
            else
            {
               int n = _scanner.readIntFix(3);

               while (n-- > 0)
               {
                  _scanner.skip(1);
                  int atom_idx = _scanner.readIntFix(3) - 1;
                  _scanner.skip(1);
                  int rbcount = _scanner.readIntFix(3);

                  if (rbcount == -1) // no ring bonds
                     _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                        new QueryMolecule::Atom(QueryMolecule::ATOM_RING_BONDS, 0)));
                  else if (rbcount == -2) // as drawn
                  {
                     int k, rbonds = 0;
                     const Vertex &vertex = _qmol->getVertex(atom_idx);

                     for (k = vertex.neiBegin(); k != vertex.neiEnd(); k = vertex.neiNext(k))
                        if (_qmol->getEdgeTopology(vertex.neiEdge(k)) == TOPOLOGY_RING)
                           rbonds++;

                     _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                        new QueryMolecule::Atom(QueryMolecule::ATOM_RING_BONDS_AS_DRAWN, rbonds)));
                  }
                  else if (rbcount > 1)
                     _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                        new QueryMolecule::Atom(QueryMolecule::ATOM_RING_BONDS, rbcount, (rbcount < 4 ? rbcount : 100))));
                  else
                     throw Error("ring bond count = %d makes no sense", rbcount);
               }
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "UNS", 3) == 0)
         {
            if (_qmol == 0)
            {
               if (!ignore_noncritical_query_features)
                  throw Error("unaturated atoms are allowed only for queries");
            }
            else
            {
               int n = _scanner.readIntFix(3);

               while (n-- > 0)
               {
                  _scanner.skip(1);
                  int atom_idx = _scanner.readIntFix(3) - 1;
                  _scanner.skip(1);
                  int unsaturation = _scanner.readIntFix(3);

                  if (unsaturation)
                     _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                        new QueryMolecule::Atom(QueryMolecule::ATOM_UNSATURATION, 0)));
               }
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "$3D", 3) == 0)
         {
            if (_qmol == 0)
            {
               if (!ignore_noncritical_query_features)
                  throw Error("3D features are allowed only for queries");
            }
            else
            {
               if (n_3d_features == -1)
               {
                  n_3d_features = _scanner.readIntFix(3);
                  _scanner.skipLine();
               }
               else
               {
                  n_3d_features--;

                  if (n_3d_features < 0)
                     throw Error("3D feature unexpected");

                  _read3dFeature2000();
               }
            }
         }
         else if (strncmp(chars, "AAL", 3) == 0)
         {
            _scanner.skip(1);
            int site_idx = _scanner.readIntFix(3) - 1;
            int n = _scanner.readIntFix(3);

            while (n-- > 0)
            {
               _scanner.skip(1);
               int atom_idx = _scanner.readIntFix(3) - 1;
               _scanner.skip(1);
               int att_type = _scanner.readIntFix(3);

               _bmol->setRSiteAttachmentOrder(site_idx, atom_idx, att_type - 1);
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "RGP", 3) == 0)
         {
            int n = _scanner.readIntFix(3);

            while (n-- > 0)
            {
               _scanner.skip(1);
               int atom_idx = _scanner.readIntFix(3) - 1;
               _scanner.skip(1);
               _bmol->allowRGroupOnRSite(atom_idx, _scanner.readIntFix(3));
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "LOG", 3) == 0)
         {
            // skip something
            _scanner.skip(3);

            _scanner.skip(1);
            int rgroup_idx = _scanner.readIntFix(3);
            _scanner.skip(1);
            int if_then = _scanner.readIntFix(3);
            _scanner.skip(1);
            int rest_h = _scanner.readIntFix(3);
            _scanner.skip(1);

            QS_DEF(Array<char>, occurrence_str);

            RGroup &rgroup = _bmol->rgroups.getRGroup(rgroup_idx);

            rgroup.if_then = if_then;
            rgroup.rest_h = rest_h;

            _scanner.readLine(occurrence_str, true);
            _readRGroupOccurrenceRanges(occurrence_str.ptr(), rgroup.occurrence);
         }
         else if (strncmp(chars, "APO", 3) == 0)
         {
            int list_length = _scanner.readIntFix(3);

            while (list_length-- > 0)
            {
               _scanner.skip(1);
               int atom_idx = _scanner.readIntFix(3) - 1;
               _scanner.skip(1);
               int att_type = _scanner.readIntFix(3);

               if (att_type == -1)
                  att_type = 3;

               for (int att_idx = 0; (1 << att_idx) <= att_type; att_idx++)
                  if (att_type & (1 << att_idx))
                     _bmol->addAttachmentPoint(att_idx + 1, atom_idx);
            }

            _scanner.skipLine();
         }
         else if (strncmp(chars, "STY", 3) == 0)
         {
            int n = _scanner.readIntFix(3);

            while (n-- > 0)
            {
               _scanner.skip(1);
               char type[4] = {0, 0, 0, 0};
               int sgroup_idx = _scanner.readIntFix(3) - 1;
               _scanner.skip(1);
               _scanner.readCharsFix(3, type);
               _sgroup_types.expandFill(sgroup_idx + 1, -1);
               _sgroup_mapping.expandFill(sgroup_idx + 1, -1);
               if (strcmp(type, "SUP") == 0)
               {
                  _sgroup_types[sgroup_idx] = _SGROUP_TYPE_SUP;
                  int idx = _bmol->superatoms.add();
                  _sgroup_mapping[sgroup_idx] = idx;
               }
               else if (strcmp(type, "DAT") == 0)
               {
                  _sgroup_types[sgroup_idx] = _SGROUP_TYPE_DAT;
                  int idx = _bmol->data_sgroups.add();
                  _sgroup_mapping[sgroup_idx] = idx;
               }
               else if (strcmp(type, "SRU") == 0)
               {
                  _sgroup_types[sgroup_idx] = _SGROUP_TYPE_SRU;
                  int idx = _bmol->repeating_units.add();
                  _sgroup_mapping[sgroup_idx] = idx;
               }
               else if (strcmp(type, "MUL") == 0)
               {
                  _sgroup_types[sgroup_idx] = _SGROUP_TYPE_MUL;
                  int idx = _bmol->multiple_groups.add();
                  _sgroup_mapping[sgroup_idx] = idx;
               }
               else if (strcmp(type, "GEN") == 0)
               {
                  _sgroup_types[sgroup_idx] = _SGROUP_TYPE_GEN;
                  int idx = _bmol->generic_sgroups.add();
                  _sgroup_mapping[sgroup_idx] = idx;
               }
               else
                  _sgroup_types[sgroup_idx] = _SGROUP_TYPE_OTHER;
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "SAL", 3) == 0 || strncmp(chars, "SBL", 3) == 0 ||
                  strncmp(chars, "SDI", 3) == 0)
         {
            _scanner.skip(1);
            int sgroup_idx = _scanner.readIntFix(3) - 1;

            if (_sgroup_mapping[sgroup_idx] >= 0)
            {
               BaseMolecule::SGroup *sgroup;

               switch (_sgroup_types[sgroup_idx])
               {
                  case _SGROUP_TYPE_DAT: sgroup = &_bmol->data_sgroups[_sgroup_mapping[sgroup_idx]]; break;
                  case _SGROUP_TYPE_SRU: sgroup = &_bmol->repeating_units[_sgroup_mapping[sgroup_idx]]; break;
                  case _SGROUP_TYPE_SUP: sgroup = &_bmol->superatoms[_sgroup_mapping[sgroup_idx]]; break;
                  case _SGROUP_TYPE_MUL: sgroup = &_bmol->multiple_groups[_sgroup_mapping[sgroup_idx]]; break;
                  case _SGROUP_TYPE_GEN: sgroup = &_bmol->generic_sgroups[_sgroup_mapping[sgroup_idx]]; break;
                  default: throw Error("internal: bad sgroup type");
               }
               
               int n = _scanner.readIntFix(3);
               
               if (strncmp(chars, "SDI", 3) == 0)
               {
                  if (n == 4) // should always be 4
                  {
                     Vec2f *brackets = sgroup->brackets.push();

                     _scanner.skipSpace();
                     brackets[0].x = _scanner.readFloat();
                     _scanner.skipSpace();
                     brackets[0].y = _scanner.readFloat();
                     _scanner.skipSpace();
                     brackets[1].x = _scanner.readFloat();
                     _scanner.skipSpace();
                     brackets[1].y = _scanner.readFloat();
                  }
               }
               else while (n-- > 0)
               {
                  _scanner.skip(1);
                  if (strncmp(chars, "SAL", 3) == 0)
                     sgroup->atoms.push(_scanner.readIntFix(3) - 1);
                  else // SBL
                     sgroup->bonds.push(_scanner.readIntFix(3) - 1);
               }
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "SDT", 3) == 0)
         {
            _scanner.skip(1);
            int sgroup_idx = _scanner.readIntFix(3) - 1;
            _scanner.skip(1);
            
            if (_sgroup_types[sgroup_idx] == _SGROUP_TYPE_DAT)
            {
               QS_DEF(Array<char>, rest);

               _scanner.readLine(rest, false);
               BufferScanner strscan(rest);
               BaseMolecule::DataSGroup &sgroup = _bmol->data_sgroups[_sgroup_mapping[sgroup_idx]];

               int k = 30;
               while (k-- > 0)
               {
                  if (strscan.isEOF())
                     break;
                  int c = strscan.readChar();
                  sgroup.description.push(c);
               }
               // Remove last spaces because description can have multiple words
               if (sgroup.description.size() > 0)
                  while (isspace(sgroup.description.top()))
                     sgroup.description.pop();

               sgroup.description.push(0);
            }
         }
         else if (strncmp(chars, "SDD", 3) == 0)
         {
            _scanner.skip(1);
            int sgroup_idx = _scanner.readIntFix(3) - 1;
            if (_sgroup_types[sgroup_idx] == _SGROUP_TYPE_DAT)
            {
               _scanner.skip(1);
               BaseMolecule::DataSGroup &sgroup = _bmol->data_sgroups[_sgroup_mapping[sgroup_idx]];

               _readSGroupDisplay(_scanner, sgroup);
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "SED", 3) == 0 || strncmp(chars, "SCD", 3) == 0)
         {
            _scanner.skip(1);
            int sgroup_idx = _scanner.readIntFix(3) - 1;
            if (_sgroup_types[sgroup_idx] == _SGROUP_TYPE_DAT)
            {
               _scanner.skip(1);
               BaseMolecule::DataSGroup &sgroup = _bmol->data_sgroups[_sgroup_mapping[sgroup_idx]];
               int len = sgroup.data.size();
               _scanner.appendLine(sgroup.data, true);

               // Remove last spaces because "SED" means end of a paragraph
               if (strncmp(chars, "SED", 3) == 0)
               {
                  if (sgroup.data.top() == 0)
                     sgroup.data.pop();
                  while (sgroup.data.size() > len)
                  {
                     if (isspace(sgroup.data.top()))
                        sgroup.data.pop();
                     else
                        break;
                  }
                  // Add new paragraph. Last '\n' will be cleaned at the end
                  sgroup.data.push('\n');
                  if (sgroup.data.top() != 0)
                     sgroup.data.push(0);
               }
            }
            else
               _scanner.skipLine();
         }
         else if (strncmp(chars, "SMT", 3) == 0)
         {
            _scanner.skip(1);
            int sgroup_idx = _scanner.readIntFix(3) - 1;
            if (_sgroup_types[sgroup_idx] == _SGROUP_TYPE_SUP)
            {
               _scanner.skip(1);
               BaseMolecule::Superatom &sup = _bmol->superatoms[_sgroup_mapping[sgroup_idx]];
               _scanner.readLine(sup.subscript, true);
            }
            else if (_sgroup_types[sgroup_idx] == _SGROUP_TYPE_MUL)
            {
               _scanner.skip(1);
               BaseMolecule::MultipleGroup &mg = _bmol->multiple_groups[_sgroup_mapping[sgroup_idx]];
               mg.multiplier = _scanner.readInt();
               _scanner.skipLine();
            }
            else if (_sgroup_types[sgroup_idx] == _SGROUP_TYPE_SRU)
            {
               _scanner.skip(1);
               BaseMolecule::RepeatingUnit &sru = _bmol->repeating_units[_sgroup_mapping[sgroup_idx]];
               _scanner.readLine(sru.subscript, true);
            }
            else
               _scanner.skipLine();
         }
         else if (strncmp(chars, "SBV", 3) == 0)
         {
            _scanner.skip(1);
            int sgroup_idx = _scanner.readIntFix(3) - 1;
            if (_sgroup_types[sgroup_idx] == _SGROUP_TYPE_SUP)
            {
               BaseMolecule::Superatom &sup = _bmol->superatoms[_sgroup_mapping[sgroup_idx]];
               _scanner.skip(1);
               sup.bond_idx = _scanner.readIntFix(3) - 1;
               _scanner.skipSpace();
               sup.bond_dir.x = _scanner.readFloat();
               _scanner.skipSpace();
               sup.bond_dir.y = _scanner.readFloat();
               int k;
               k = 1;
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "SPA", 3) == 0)
         {
            _scanner.skip(1);
            int sgroup_idx = _scanner.readIntFix(3) - 1;

            if (_sgroup_types[sgroup_idx] == _SGROUP_TYPE_MUL)
            {
               BaseMolecule::MultipleGroup &mg = _bmol->multiple_groups[_sgroup_mapping[sgroup_idx]];
               int n = _scanner.readIntFix(3);
               while (n-- > 0)
               {
                  _scanner.skip(1);
                  mg.parent_atoms.push(_scanner.readIntFix(3) - 1);
               }
            }
            _scanner.skipLine();
         }
         else if (strncmp(chars, "SCN", 3) == 0)
         {
            // The format is the following: M SCNnn8 sss ttt ...
            int n = _scanner.readIntFix(3);

            bool need_skip_line = true;
            while (n-- > 0)
            {
               _scanner.skip(1);
               int sgroup_idx = _scanner.readIntFix(3) - 1;

               char id[4];
               _scanner.skip(1);
               _scanner.readCharsFix(3, id);

               if (_sgroup_types[sgroup_idx] == _SGROUP_TYPE_SRU)
               {
                  BaseMolecule::RepeatingUnit &ru = _bmol->repeating_units[_sgroup_mapping[sgroup_idx]];

                  if (strncmp(id, "HH", 2) == 0)
                     ru.connectivity = BaseMolecule::RepeatingUnit::HEAD_TO_HEAD;
                  else if (strncmp(id, "HT", 2) == 0)
                     ru.connectivity = BaseMolecule::RepeatingUnit::HEAD_TO_TAIL;
                  else if (strncmp(id, "EU", 2) == 0)
                     ru.connectivity = BaseMolecule::RepeatingUnit::EITHER;
                  else
                  {
                     id[3] = 0;
                     throw Error("Undefined Sgroup connectivity: '%s'", id);
                  }
                  if (id[2] == '\n')
                  {
                     if (n != 0)
                        throw Error("Unexpected end of M SCN");
                     else
                        // In some molfiles last space is not written
                        need_skip_line = false;
                  }
               }
            }
            if (need_skip_line)
               _scanner.skipLine();
         }
         else if (strncmp(chars, "MRV", 3) == 0)
         {
            _scanner.readLine(str, false);
            BufferScanner rest(str);

            try
            {
               rest.skip(1);
               rest.readCharsFix(3, chars);
               if (strncmp(chars, "SMA", 3) == 0)
               {
                  // Marvin's "SMARTS in Molfile" extension
                  if (_qmol == 0)
                  {
                     if (!ignore_noncritical_query_features)
                        throw Error("SMARTS notation allowed only for query molecules");
                  }
                  else
                  {
                     rest.skip(1);
                     int idx = rest.readIntFix(3) - 1;
                     rest.skip(1);

                     QS_DEF(QueryMolecule, smartsmol);
                     SmilesLoader loader(rest);

                     smartsmol.clear();
                     loader.smarts_mode = true;
                     loader.loadQueryMolecule(smartsmol);

                     if (smartsmol.vertexCount() != 1)
                        throw Error("expected 1 atom in SMARTS expression, got %d", smartsmol.vertexCount());

                     _qmol->resetAtom(idx, smartsmol.releaseAtom(smartsmol.vertexBegin()));
                  }
               }
            }
            catch (Scanner::Error &)
            {
            }
         }
         else
            _scanner.skipLine();
      }
      else if (c == 'A')
      {
         QS_DEF(Array<char>, pseudo);

         _scanner.skip(2);
         int atom_idx = _scanner.readIntFix(3);

         atom_idx--;
         _scanner.skipLine();
         _scanner.readLine(pseudo, true);
         _preparePseudoAtomLabel(pseudo);

         if (_mol != 0)
            _mol->setPseudoAtom(atom_idx, pseudo.ptr());
         else
            _qmol->resetAtom(atom_idx, QueryMolecule::Atom::und(_qmol->releaseAtom(atom_idx),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_PSEUDO, pseudo.ptr())));

         _atom_types[atom_idx] = _ATOM_PSEUDO;
      }
      else
         _scanner.skipLine();
   }

   // Remove last new lines for data SGroups
   int dgroups_count = _bmol->data_sgroups.size();
   for (int i = 0; i < dgroups_count; i++)
   {
      BaseMolecule::DataSGroup &sgroup = _bmol->data_sgroups[i];
      if (sgroup.data.size() > 2 && sgroup.data.top(1) == '\n')
      {
         sgroup.data.pop();
         sgroup.data.top() = 0;
      }
   }

   if (_qmol == 0)
      for (int atom_idx = 0; atom_idx < _atoms_num; atom_idx++)
         if (_atom_types[atom_idx] == _ATOM_A)
            throw Error("'any' atoms are allowed only for queries");
}

void MolfileLoader::_appendQueryAtom (const char *atom_label, AutoPtr<QueryMolecule::Atom> &atom)
{
   int atom_number = Element::fromString2(atom_label);
   AutoPtr<QueryMolecule::Atom> cur_atom;
   if (atom_number != -1)
      cur_atom.reset(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, atom_number));
   else
      cur_atom.reset(new QueryMolecule::Atom(QueryMolecule::ATOM_PSEUDO, atom_label));

   if (atom.get() == 0)
      atom.reset(cur_atom.release());
   else
      atom.reset(QueryMolecule::Atom::oder(atom.release(), cur_atom.release()));
}

void MolfileLoader::_convertCharge (int value, int &charge, int &radical)
{
   switch (value)
   {
      case 1: charge = 3; break;
      case 2: charge = 2; break;
      case 3: charge = 1; break;
      case 4: radical = 2; break;
      case 5: charge = -1; break;
      case 6: charge = -2; break;
      case 7: charge = -3; break;
   }
}

void MolfileLoader::_read3dFeature2000 ()
{
   // read 3D feature ID (see MDL ctfile documentation)
   int feature_id = _scanner.readIntFix(3);

   _scanner.skipLine();

   Molecule3dConstraints *constraints = &_qmol->spatial_constraints;

   if (constraints->end() == 0)
      constraints->init();

   switch (feature_id)
   {
      case -1: // point defined by 2 points and distance
      {
         AutoPtr<Molecule3dConstraints::PointByDistance> constr;

         constr.create();

         _scanner.skip(6);
         constr->beg_id = _scanner.readIntFix(3) - 1;
         constr->end_id = _scanner.readIntFix(3) - 1;
         constr->distance = _scanner.readFloatFix(10);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -2: // point defined by 2 points and percentage
      {
         AutoPtr<Molecule3dConstraints::PointByPercentage> constr;

         constr.create();

         _scanner.skip(6);
         constr->beg_id = _scanner.readIntFix(3) - 1;
         constr->end_id = _scanner.readIntFix(3) - 1;
         constr->percentage = _scanner.readFloatFix(10);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -3: // point defined by point, normal line, and distance
      {
         AutoPtr<Molecule3dConstraints::PointByNormale> constr;

         constr.create();

         _scanner.skip(6);
         constr->org_id = _scanner.readIntFix(3) - 1;
         constr->norm_id = _scanner.readIntFix(3) - 1;
         constr->distance = _scanner.readFloatFix(10);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -4: // line defined by 2 or more points (best fit line if more than 2 points)
      {
         AutoPtr<Molecule3dConstraints::BestFitLine> constr;

         constr.create();

         _scanner.skip(6);

         int amount = _scanner.readIntFix(3);
         if (amount < 2)
            throw Error("invalid points amount in M $3D-4 feature");

         constr->max_deviation = _scanner.readFloatFix(10);
         _scanner.skipLine();
         _scanner.skip(6);

         while (amount-- > 0)
            constr->point_ids.push(_scanner.readIntFix(3) - 1);

         _scanner.skipLine();
         constraints->add(constr.release());
         break;
      }
      case -5: // plane defined by 3 or more points (best fit line if more than 3 points)
      {
         AutoPtr<Molecule3dConstraints::BestFitPlane> constr;

         constr.create();

         _scanner.skip(6);

         int amount = _scanner.readIntFix(3);

         if (amount < 3)
            throw Error("invalid points amount in M $3D-5 feature");

         constr->max_deviation = _scanner.readFloatFix(10);
         _scanner.skipLine();
         _scanner.skip(6);

         while (amount-- > 0)
            constr->point_ids.push(_scanner.readIntFix(3) - 1);

         _scanner.skipLine();
         constraints->add(constr.release());
         break;
      }
      case -6: // plane defined by point and line
      {
         AutoPtr<Molecule3dConstraints::PlaneByPoint> constr;

         constr.create();

         _scanner.skip(6);
         constr->point_id = _scanner.readIntFix(3) - 1;
         constr->line_id = _scanner.readIntFix(3) - 1;
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -7: // centroid defined by points
      {
         AutoPtr<Molecule3dConstraints::Centroid> constr;

         constr.create();
         _scanner.skip(6);

         int amount = _scanner.readIntFix(3);

         if (amount < 1)
            throw Error("invalid amount of points for centroid: %d", amount);

         _scanner.skipLine();
         _scanner.skip(6);

         while (amount-- > 0)
            constr->point_ids.push(_scanner.readIntFix(3) - 1);

         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -8: // normal line defined by point and plane
      {
         AutoPtr<Molecule3dConstraints::Normale> constr;

         constr.create();
         _scanner.skip(6);
         constr->point_id = _scanner.readIntFix(3) - 1;
         constr->plane_id = _scanner.readIntFix(3) - 1;

         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -9: // distance defined by 2 points and range
      {
         AutoPtr<Molecule3dConstraints::DistanceByPoints> constr;

         constr.create();
         _scanner.skip(6);
         constr->beg_id = _scanner.readIntFix(3) - 1;
         constr->end_id = _scanner.readIntFix(3) - 1;
         constr->bottom = _scanner.readFloatFix(10);
         constr->top    = _scanner.readFloatFix(10);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -10: // distance defined by point, line and range
      {
         AutoPtr<Molecule3dConstraints::DistanceByLine> constr;

         constr.create();

         _scanner.skip(6);
         constr->point_id = _scanner.readIntFix(3) - 1;
         constr->line_id = _scanner.readIntFix(3) - 1;
         constr->bottom = _scanner.readFloatFix(10);
         constr->top = _scanner.readFloatFix(10);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -11: // distance defined by point, plane and range
      {
         AutoPtr<Molecule3dConstraints::DistanceByPlane> constr;

         constr.create();
         _scanner.skip(6);
         constr->point_id = _scanner.readIntFix(3) - 1;
         constr->plane_id = _scanner.readIntFix(3) - 1;
         constr->bottom = _scanner.readFloatFix(10);
         constr->top = _scanner.readFloatFix(10);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -12: // angle defined by 3 points and range
      {
         AutoPtr<Molecule3dConstraints::AngleByPoints> constr;

         constr.create();
         _scanner.skip(6);
         constr->point1_id = _scanner.readIntFix(3) - 1;
         constr->point2_id = _scanner.readIntFix(3) - 1;
         constr->point3_id = _scanner.readIntFix(3) - 1;
         constr->bottom = (float)(_scanner.readFloatFix(10) * M_PI / 180);
         constr->top    = (float)(_scanner.readFloatFix(10) * M_PI / 180);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -13: // angle defined by 2 lines and range
      {
         AutoPtr<Molecule3dConstraints::AngleByLines> constr;

         constr.create();
         _scanner.skip(6);
         constr->line1_id = _scanner.readIntFix(3) - 1;
         constr->line2_id = _scanner.readIntFix(3) - 1;
         constr->bottom = (float)(_scanner.readFloatFix(10) * M_PI / 180);
         constr->top    = (float)(_scanner.readFloatFix(10) * M_PI / 180);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -14: // angles defined by 2 planes and range
      {
         AutoPtr<Molecule3dConstraints::AngleByPlanes> constr;

         constr.create();
         _scanner.skip(6);
         constr->plane1_id = _scanner.readIntFix(3) - 1;
         constr->plane2_id = _scanner.readIntFix(3) - 1;
         constr->bottom = (float)(_scanner.readFloatFix(10) * M_PI / 180);
         constr->top    = (float)(_scanner.readFloatFix(10) * M_PI / 180);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -15: // dihedral angle defined by 4 points
      {
         AutoPtr<Molecule3dConstraints::AngleDihedral> constr;

         constr.create();
         _scanner.skip(6);
         constr->point1_id = _scanner.readIntFix(3) - 1;
         constr->point2_id = _scanner.readIntFix(3) - 1;
         constr->point3_id = _scanner.readIntFix(3) - 1;
         constr->point4_id = _scanner.readIntFix(3) - 1;
         constr->bottom = (float)(_scanner.readFloatFix(10) * M_PI / 180);
         constr->top    = (float)(_scanner.readFloatFix(10) * M_PI / 180);
         _scanner.skipLine();

         constraints->add(constr.release());
         break;
      }
      case -16: // exclusion sphere defines by points and distance
      {
         AutoPtr<Molecule3dConstraints::ExclusionSphere> constr;

         int allowed_atoms_amount;
         Array<int> allowed_atoms;

         constr.create();
         _scanner.skip(6);
         constr->center_id = _scanner.readIntFix(3) - 1;
         constr->allow_unconnected = (_scanner.readIntFix(3) != 0);
         allowed_atoms_amount = _scanner.readIntFix(3);
         constr->radius = (float)(_scanner.readFloatFix(10));

         if (allowed_atoms_amount > 0)
         {
            _scanner.skipLine();
            _scanner.skip(6);

            while (allowed_atoms_amount-- > 0)
               constr->allowed_atoms.push(_scanner.readIntFix(3) - 1);
         }

         _scanner.skipLine();
         constraints->add(constr.release());
         break;
      }
      case -17: // fixed atoms
      {
         _scanner.skip(6);
         int amount = _scanner.readIntFix(3);
         _scanner.skipLine();
         _scanner.skip(6);

         while (amount-- > 0)
            _qmol->fixed_atoms.push(_scanner.readIntFix(3) - 1);

         _scanner.skipLine();
         break;
      }
      default:
         throw Error("unknown 3D feature in createFromMolfile: %d", feature_id);
   }
}

void MolfileLoader::_readRGroupOccurrenceRanges (const char *str, Array<int> &ranges)
{
   int beg = -1, end = -1;
   int add_beg = 0, add_end = 0;

   while (*str != 0)
   {
      if (*str == '>')
      {
         end = 0xFFFF;
         add_beg = 1;
      } else if (*str == '<')
      {
         beg = 0;
         add_end = -1;
      } else if (isdigit(*str))
      {
         sscanf(str, "%d", beg == -1 ? &beg : &end);
         while (isdigit(*str))
            str++;
         continue;
      } else if (*str == ',')
      {
         if (end == -1)
            end = beg;
         else
            beg += add_beg, end += add_end;
         ranges.push((beg << 16) | end);
         beg = end = -1;
         add_beg = add_end = 0;
      }
      str++;
   }

   if (beg == -1 && end == -1)
      return;

   if (end == -1)
      end = beg;
   else
      beg += add_beg, end += add_end;
   ranges.push((beg << 16) | end);
}

int MolfileLoader::_asc_cmp_cb (int &v1, int &v2, void *context)
{
   return v2 - v1;
}

void MolfileLoader::_postLoad ()
{
   int i;

   for (i = _bmol->vertexBegin(); i < _bmol->vertexEnd(); i = _bmol->vertexNext(i))
   {
      // Update attachment orders for rgroup bonds if they were not specified explicitly
      if (!_bmol->isRSite(i))
         continue;

      const Vertex &vertex = _bmol->getVertex(i);

      if (vertex.degree() == 1 && _bmol->getRSiteAttachmentPointByOrder(i, 0) == -1)
         _bmol->setRSiteAttachmentOrder(i, vertex.neiVertex(vertex.neiBegin()), 0);
      else if (vertex.degree() == 2 &&
              (_bmol->getRSiteAttachmentPointByOrder(i, 0) == -1 ||
               _bmol->getRSiteAttachmentPointByOrder(i, 1) == -1))
      {
         int nei_idx_1 = vertex.neiVertex(vertex.neiBegin());
         int nei_idx_2 = vertex.neiVertex(vertex.neiNext(vertex.neiBegin()));

         _bmol->setRSiteAttachmentOrder(i, __min(nei_idx_1, nei_idx_2), 0);
         _bmol->setRSiteAttachmentOrder(i, __max(nei_idx_1, nei_idx_2), 1);
      }
      else if (vertex.degree() > 2)
      {
         int j;

         for (j = 0; j < vertex.degree(); j++)
            if (_bmol->getRSiteAttachmentPointByOrder(i, j) == -1)
            {
               QS_DEF(Array<int>, nei_indices);
               nei_indices.clear();

               for (int nei_idx = vertex.neiBegin(); nei_idx < vertex.neiEnd(); nei_idx = vertex.neiNext(nei_idx))
                  nei_indices.push(vertex.neiVertex(nei_idx));

               nei_indices.qsort(_asc_cmp_cb, 0);

               for (int order = 0; order < vertex.degree(); order++)
                  _bmol->setRSiteAttachmentOrder(i, nei_indices[order], order);
               break;
            }
      }
   }

   if (_mol != 0)
   {
      int k;
      for (k = 0; k < _atoms_num; k++)
         if (_hcount[k] > 0)
            _mol->setImplicitH(k, _hcount[k] - 1);

   }

   if (_qmol != 0)
   {
      for (i = _qmol->edgeBegin(); i < _qmol->edgeEnd(); i = _qmol->edgeNext(i))
      {
         if (_ignore_cistrans[i])
             continue;

         const Edge &edge = _qmol->getEdge(i);

         if ((_stereo_care_atoms[edge.beg] && _stereo_care_atoms[edge.end]) ||
               _stereo_care_bonds[i])
         {
             // in fragments such as C-C=C-C=C-C, the middle single bond
             // has both ends 'stereo care', but should not be considered
             // as 'stereo care' itself
             if (MoleculeCisTrans::isGeomStereoBond(*_bmol, i, 0, true))
               _qmol->setBondStereoCare(i, true);
         }
      }

      int k;
      for (k = 0; k < _atoms_num; k++)
      {
         int expl_h = 0;
         
         if (_hcount[k] >= 0)
         {
            // count explicit hydrogens
            const Vertex &vertex = _bmol->getVertex(k);
            int i;
            
            for (i = vertex.neiBegin(); i != vertex.neiEnd(); i = vertex.neiNext(i))
            {
               if (_bmol->getAtomNumber(vertex.neiVertex(i)) == ELEM_H)
                  expl_h++;
            }
         }

         if (_hcount[k] == 1) 
         {
            // no hydrogens unless explicitly drawn
            _qmol->resetAtom(k, QueryMolecule::Atom::und(_qmol->releaseAtom(k),
               new QueryMolecule::Atom(QueryMolecule::ATOM_TOTAL_H, expl_h)));
         }
         else if (_hcount[k] > 1)
         {
            // (_hcount[k] - 1) or more atoms in addition to explicitly drawn
            // no hydrogens unless explicitly drawn
            _qmol->resetAtom(k, QueryMolecule::Atom::und(_qmol->releaseAtom(k),
               new QueryMolecule::Atom(QueryMolecule::ATOM_TOTAL_H, expl_h + _hcount[k] - 1, 100)));
         }
      }
   }

   // Some "either" bonds may mean not "either stereocenter", but
   // "either cis-trans", or "connected to either cis-trans".
   for (i = 0; i < _bonds_num; i++)
      if (_bmol->getBondDirection(i) == BOND_EITHER)
      {
         if (MoleculeCisTrans::isGeomStereoBond(*_bmol, i, 0, true))
         {
            _ignore_cistrans[i] = 1;
            _sensible_bond_directions[i] = 1;
         }
         else
         {
            int k;
            const Vertex &v = _bmol->getVertex(_bmol->getEdge(i).beg);

            for (k = v.neiBegin(); k != v.neiEnd(); k = v.neiNext(k))
            {
               if (MoleculeCisTrans::isGeomStereoBond(*_bmol, v.neiEdge(k), 0, true))
               {
                  _ignore_cistrans[v.neiEdge(k)] = 1;
                  _sensible_bond_directions[i] = 1;
                  break;
               }
            }
         }
      }

   _bmol->stereocenters.buildFromBonds(ignore_stereocenter_errors, _sensible_bond_directions.ptr());
   _bmol->allene_stereo.buildFromBonds(ignore_stereocenter_errors, _sensible_bond_directions.ptr());

   if (!_chiral)
      for (i = 0; i < _atoms_num; i++)
      {
         int type = _bmol->stereocenters.getType(i);

         if (type == MoleculeStereocenters::ATOM_ABS)
            _bmol->stereocenters.setType(i, MoleculeStereocenters::ATOM_AND, 1);
      }
   
   for (i = 0; i < _atoms_num; i++)
      if (_stereocenter_types[i] > 0)
      {
         if (_bmol->stereocenters.getType(i) == 0)
         {
            if (!ignore_stereocenter_errors)
               throw Error("stereo type specified for atom #%d, but the bond "
                    "directions does not say that it is a stereocenter", i);
         }
         else
            _bmol->stereocenters.setType(i, _stereocenter_types[i], _stereocenter_groups[i]);
      }

   if (!ignore_stereocenter_errors)
      for (i = 0; i < _bonds_num; i++)
         if (_bmol->getBondDirection(i) > 0 && !_sensible_bond_directions[i])
            throw Error("direction of bond #%d makes no sense", i);

   _bmol->cis_trans.build(_ignore_cistrans.ptr());

   int n_rgroups = _bmol->rgroups.getRGroupCount();
   for (i = 1; i <= n_rgroups; i++)
      if (_bmol->rgroups.getRGroup(i).occurrence.size() == 0 &&
          _bmol->rgroups.getRGroup(i).fragments.size() > 0)
         _bmol->rgroups.getRGroup(i).occurrence.push((1 << 16) | 0xFFFF);

   _bmol->have_xyz = true;
}

void MolfileLoader::_readRGroups2000 ()
{
   MoleculeRGroups *rgroups = &_bmol->rgroups;
   
   // read groups
   while (!_scanner.isEOF())
   {
      char chars[5];

      chars[4] = 0;
      _scanner.readCharsFix(4, chars);

      if (strncmp(chars, "$RGP", 4) == 0)
      {
         _scanner.skipLine();
         _scanner.skipSpace();

         int rgroup_idx = _scanner.readInt();
         RGroup &rgroup = rgroups->getRGroup(rgroup_idx);

         _scanner.skipLine();
         while (!_scanner.isEOF())
         {
            char rgp_chars[6];
            rgp_chars[5] = 0;
            _scanner.readCharsFix(5, rgp_chars);

            if (strncmp(rgp_chars, "$CTAB", 5) == 0)
            {
               _scanner.skipLine();
               AutoPtr<BaseMolecule> fragment(_bmol->neu());

               MolfileLoader loader(_scanner);

               loader._bmol = fragment.get();
               if (_bmol->isQueryMolecule())
               {
                  loader._qmol = &loader._bmol->asQueryMolecule();
                  loader._mol = 0;
               }
               else
               {
                  loader._mol = &loader._bmol->asMolecule();
                  loader._qmol = 0;
               }
               loader._readCtabHeader();
               loader._readCtab2000();
               if (loader._rgfile)
                  loader._readRGroups2000();
               loader._postLoad();

               rgroup.fragments.add(fragment.release());
            }
            else if (strncmp(rgp_chars, "$END ", 5) == 0)
            {
               rgp_chars[3] = 0;
               _scanner.readCharsFix(3, rgp_chars);

               _scanner.skipLine();
               if (strncmp(rgp_chars, "RGP", 3) == 0)
                  break;
            }
            else
               _scanner.skipLine();
         }
      }
      else if (strncmp(chars, "$END", 4) == 0)
      {
         chars[4] = 0;
         _scanner.readCharsFix(4, chars);
         _scanner.skipLine();
         if (strncmp(chars, " MOL", 4) == 0)
            break;
      } else
         _scanner.skipLine();
   }
}

void MolfileLoader::_readCtab3000 ()
{
   QS_DEF(Array<char>, str);

   _scanner.readLine(str, true);
   if (strncmp(str.ptr(), "M  V30 BEGIN CTAB", 17) != 0)
      throw Error("error reading CTAB block header");

   str.clear_resize(14);
   _scanner.read(14, str.ptr());
   if (strncmp(str.ptr(), "M  V30 COUNTS ", 14) != 0)
      throw Error("error reading COUNTS line");

   int i, nsgroups, n3d, chiral_int;

   _scanner.readLine(str, true);
   if (sscanf(str.ptr(), "%d %d %d %d %d",
              &_atoms_num, &_bonds_num, &nsgroups, &n3d, &chiral_int) < 5)
      throw Error("error parsing COUNTS line");

   _chiral = (chiral_int != 0);

   _init();

   _scanner.readLine(str, true);
   if (strncmp(str.ptr(), "M  V30 BEGIN ATOM", 14) != 0)
      throw Error("Error reading ATOM block header");

   for (i = 0; i < _atoms_num; i++)
   {
      _readMultiString(str);
      BufferScanner strscan(str.ptr());

      int &atom_type = _atom_types.push();

      _hcount.push(0);

      atom_type = _ATOM_ELEMENT;

      int isotope = 0;
      int label = 0;
      AutoPtr<QueryMolecule::Atom> query_atom;

      strscan.readInt1(); // atom index -- ignored

      QS_DEF(Array<char>, buf);

      strscan.readWord(buf, " [");

      char stopchar = strscan.readChar();

      if (stopchar == '[')
      {
         if (_qmol == 0)
            throw Error("atom list is allowed only for queries");

         if (buf[0] == 0)
            atom_type = _ATOM_LIST;
         else if (strcmp(buf.ptr(), "NOT") == 0)
            atom_type = _ATOM_NOTLIST;
         else
            throw Error("bad word: %s", buf.ptr());

         bool was_a = false, was_q = false;

         while (1)
         {
            strscan.readWord(buf, ",]");
            stopchar = strscan.readChar();

            if (was_a)
               throw Error("'A' inside atom list, if present, must be single");
            if (was_q)
               throw Error("'Q' inside atom list, if present, must be single");

            if (buf.size() == 2 && buf[0] == 'A')
            {
               was_a = true;
               atom_type = _ATOM_A;
            }
            else if (buf.size() == 2 && buf[0] == 'Q')
            {
               was_q = true;
               atom_type = _ATOM_Q;
            }
            else
            {
               _appendQueryAtom(buf.ptr(), query_atom);
            }

            if (stopchar == ']')
               break;
         }
      }
      else if (buf.size() == 2 && buf[0] == 'D')
      {
         label = ELEM_H;
         isotope = 2;
      }
      else if (buf.size() == 2 && buf[0] == 'T')
      {
         label = ELEM_H;
         isotope = 3;
      }
      else if (buf.size() == 2 && buf[0] == 'Q')
      {
         if (_qmol == 0)
            throw Error("'Q' atom is allowed only for queries");

         atom_type = _ATOM_Q;
      }
      else if (buf.size() == 2 && buf[0] == 'A')
      {
         if (_qmol == 0)
            throw Error("'A' atom is allowed only for queries");

         atom_type = _ATOM_A;
      }
      else if (buf.size() == 2 && buf[0] == 'X' && !treat_x_as_pseudoatom)
      {
         if (_qmol == 0)
            throw Error("'X' atom is allowed only for queries");

         atom_type = _ATOM_X;
      }
      else if (buf.size() == 3 && buf[0] == 'R' && buf[1] == '#')
      {
         atom_type = _ATOM_R;
         label = ELEM_RSITE;
      }
      else
      {
         label = Element::fromString2(buf.ptr());

         if (label == -1)
            _atom_types[i] = _ATOM_PSEUDO;
      }

      strscan.skipSpace();
      float x = strscan.readFloat();
      strscan.skipSpace();
      float y = strscan.readFloat();
      strscan.skipSpace();
      float z = strscan.readFloat();
      strscan.skipSpace();
      int aamap = strscan.readInt1();

      if (_mol != 0)
      {
         _mol->addAtom(label);
         if (atom_type == _ATOM_PSEUDO)
         {
            _preparePseudoAtomLabel(buf);
            _mol->setPseudoAtom(i, buf.ptr());
         }
      }
      else
      {
         if (atom_type == _ATOM_LIST)
            _qmol->addAtom(query_atom.release());
         else if (atom_type == _ATOM_NOTLIST)
            _qmol->addAtom(QueryMolecule::Atom::nicht(query_atom.release()));
         else if (atom_type == _ATOM_ELEMENT)
            _qmol->addAtom(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, label));
         else if (atom_type == _ATOM_PSEUDO)
            _qmol->addAtom(new QueryMolecule::Atom(QueryMolecule::ATOM_PSEUDO, buf.ptr()));
         else if (atom_type == _ATOM_A)
            _qmol->addAtom(QueryMolecule::Atom::nicht(
                        new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_H)));
         else if (atom_type == _ATOM_X)
         {
            AutoPtr<QueryMolecule::Atom> atom(new QueryMolecule::Atom());
            
            atom->type = QueryMolecule::OP_OR;
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_F));
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_Cl));
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_Br));
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_I));
            atom->children.add(new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_At));
            _qmol->addAtom(atom.release());
         }
         else if (atom_type == _ATOM_Q)
            _qmol->addAtom(QueryMolecule::Atom::und(
                           QueryMolecule::Atom::nicht(
                              new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_H)),
                           QueryMolecule::Atom::nicht(
                              new QueryMolecule::Atom(QueryMolecule::ATOM_NUMBER, ELEM_C))));
         else // _ATOM_R
            _qmol->addAtom(new QueryMolecule::Atom(QueryMolecule::ATOM_RSITE, 0));
      }

      int hcount = 0;
      int irflag = 0;
      int ecflag = 0;
      int radical = 0;

      while (true)
      {
         strscan.skipSpace();
         if (strscan.isEOF())
            break;

         QS_DEF(Array<char>, prop_arr);
         strscan.readWord(prop_arr, "=");

         strscan.skip(1);
         const char *prop = prop_arr.ptr();

         if (strcmp(prop, "CHG") == 0)
         {
            int charge = strscan.readInt1();

            if (_mol != 0)
               _mol->setAtomCharge_Silent(i, charge);
            else
            {
               _qmol->resetAtom(i, QueryMolecule::Atom::und(_qmol->releaseAtom(i),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_CHARGE, charge)));
            }
         }
         else if (strcmp(prop, "RAD") == 0)
         {
            radical = strscan.readInt1();

            if (_qmol != 0)
            {
               _qmol->resetAtom(i, QueryMolecule::Atom::und(_qmol->releaseAtom(i),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_RADICAL, radical)));
            }
         }
         else if (strcmp(prop, "CFG") == 0)
         {
            strscan.readInt1();
            //int cfg = strscan.readInt1();

            //if (cfg == 3)
            //   _stereocenter_types[idx] = MoleculeStereocenters::ATOM4;
         }
         else if (strcmp(prop, "MASS") == 0)
         {
            isotope = strscan.readInt1();
         }
         else if (strcmp(prop, "VAL") == 0)
         {
            int valence = strscan.readInt1();

            if (valence == -1)
               valence = 0;

            if (_mol != 0)
            {
               _mol->setExplicitValence(i, valence);
            }
            else
            {
               _qmol->resetAtom(i, QueryMolecule::Atom::und(_qmol->releaseAtom(i),
                     new QueryMolecule::Atom(QueryMolecule::ATOM_VALENCE, valence)));
            }

         }
         else if (strcmp(prop, "HCOUNT") == 0)
         {
            int hcount = strscan.readInt1();
            
            if (hcount == -1)
               _hcount[i] = 1;
            else if (hcount > 0)
               _hcount[i] = hcount + 1; // to comply to the code in _postLoad()
            else
               throw Error("invalid HCOUNT value: %d", hcount);
         }
         else if (strcmp(prop, "STBOX") == 0)
            _stereo_care_atoms[i] = strscan.readInt1();
         else if (strcmp(prop, "INVRET") == 0)
            irflag = strscan.readInt1();
         else if (strcmp(prop, "EXACHG") == 0)
            ecflag = strscan.readInt1();
         else if (strcmp(prop, "SUBST") == 0)
         {
            if (_qmol == 0)
               throw Error("substitution count is allowed only for queries");

            int subst = strscan.readInt1();

            if (subst != 0)
            {
               if (subst == -1)
                  _qmol->resetAtom(i, QueryMolecule::Atom::und(_qmol->releaseAtom(i),
                           new QueryMolecule::Atom(QueryMolecule::ATOM_SUBSTITUENTS, 0)));
               else if (subst == -2)
                  throw Error("Query substitution count as drawn is not supported yet (SUBST=%d)", subst);
               else if (subst > 0)
                  _qmol->resetAtom(i, QueryMolecule::Atom::und(_qmol->releaseAtom(i),
                           new QueryMolecule::Atom(QueryMolecule::ATOM_SUBSTITUENTS,
                                  subst, (subst < 6 ? subst : 100))));
               else
                  throw Error("invalid SUBST value: %d", subst);
            }
         }
         else if (strcmp(prop, "UNSAT") == 0)
         {
            if (_qmol == 0)
               throw Error("unsaturation flag is allowed only for queries");

            bool unsat = (strscan.readInt1() > 0);

            if (unsat)
               _qmol->resetAtom(i, QueryMolecule::Atom::und(_qmol->releaseAtom(i),
                  new QueryMolecule::Atom(QueryMolecule::ATOM_UNSATURATION, 0)));
         }
         else if (strcmp(prop, "RBCNT") == 0)
         {
            if (_qmol == 0)
               throw Error("ring bond count is allowed only for queries");
            
            int rb = strscan.readInt1();

            if (rb != 0)
            {
               if (rb == -1)
                  rb = 0;

               if (rb > 1)
                  _qmol->resetAtom(i, QueryMolecule::Atom::und(_qmol->releaseAtom(i),
                           new QueryMolecule::Atom(QueryMolecule::ATOM_RING_BONDS, rb, (rb < 4 ? rb : 100))));
               else
                  throw Error("invalid RBCNT value: %d", rb);
            }
         }
         else if (strcmp(prop, "RGROUPS") == 0)
         {
            int n_rg;

            strscan.skip(1); // skip '('
            n_rg = strscan.readInt1();
            while (n_rg-- > 0)
               _bmol->allowRGroupOnRSite(i, strscan.readInt1());
         }
         else if (strcmp(prop, "ATTCHPT") == 0)
         {
            int att_type = strscan.readInt1();

            if (att_type == -1)
               att_type = 3;

            for (int att_idx = 0; (1 << att_idx) <= att_type; att_idx++)
               if (att_type & (1 << att_idx))
                  _bmol->addAttachmentPoint(att_idx + 1, i);
         }
         else if (strcmp(prop, "ATTCHORD") == 0)
         {
            int n_items, nei_idx, att_type;

            strscan.skip(1); // skip '('
            n_items = strscan.readInt1() / 2;
            while (n_items-- > 0)
            {
               nei_idx = strscan.readInt1();
               att_type = strscan.readInt1();
               _bmol->setRSiteAttachmentOrder(i, nei_idx - 1, att_type - 1);
            }
         }
         else
         {
            throw Error("unsupported property of CTAB3000: %s", prop);
         }
      }

      if (isotope != 0)
      {
         if (_mol != 0)
            _mol->setAtomIsotope(i, isotope);
         else
            _qmol->resetAtom(i, QueryMolecule::Atom::und(_qmol->releaseAtom(i),
                  new QueryMolecule::Atom(QueryMolecule::ATOM_ISOTOPE, isotope)));
      }

      if (_mol != 0)
         _mol->setAtomRadical(i, radical);

      if (reaction_atom_inversion != 0)
         reaction_atom_inversion->at(i) = irflag;

      if (reaction_atom_exact_change != 0)
         reaction_atom_exact_change->at(i) = ecflag;

      if (reaction_atom_mapping != 0)
         reaction_atom_mapping->at(i) = aamap;

      _bmol->setAtomXyz(i, x, y, z);
   }

   _scanner.readLine(str, true);
   if (strncmp(str.ptr(), "M  V30 END ATOM", 15) != 0)
      throw Error("Error reading ATOM block footer");

   _scanner.readLine(str, true);
   if (strncmp(str.ptr(), "M  V30 BEGIN BOND", 17) != 0)
   {  
      if (_bonds_num > 0)
         throw Error("Error reading BOND block header");
   }
   else
   {
      for (i = 0; i < _bonds_num; i++)
      {
         int reacting_center = 0;

         _readMultiString(str);
         BufferScanner strscan(str.ptr());

         strscan.readInt1(); // bond index -- ignored

         int order = strscan.readInt1();
         int beg = strscan.readInt1() - 1;
         int end = strscan.readInt1() - 1;

         if (_mol != 0)
         {
            if (order == BOND_SINGLE || order == BOND_DOUBLE ||
                order == BOND_TRIPLE || order == BOND_AROMATIC)
               _mol->addBond_Silent(beg, end, order);
            else if (order == _BOND_SINGLE_OR_DOUBLE)
               throw Error("'single or double' bonds are allowed only for queries");
            else if (order == _BOND_SINGLE_OR_AROMATIC)
               throw Error("'single or aromatic' bonds are allowed only for queries");
            else if (order == _BOND_DOUBLE_OR_AROMATIC)
               throw Error("'double or aromatic' bonds are allowed only for queries");
            else if (order == _BOND_ANY)
               throw Error("'any' bonds are allowed only for queries");
            else
               throw Error("unknown bond type: %d", order);
         }
         else
         {
            AutoPtr<QueryMolecule::Bond> bond;

            if (order == BOND_SINGLE || order == BOND_DOUBLE ||
                order == BOND_TRIPLE || order == BOND_AROMATIC)
               bond.reset(new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, order));
            else if (order == _BOND_SINGLE_OR_DOUBLE)
            {
               bond.reset(QueryMolecule::Bond::und(
                  QueryMolecule::Bond::nicht(
                    new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_AROMATIC)),
                  QueryMolecule::Bond::oder(
                    new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_SINGLE),
                    new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_DOUBLE))));
            }
            else if (order == _BOND_SINGLE_OR_AROMATIC)
               bond.reset(QueryMolecule::Bond::oder(
               new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_SINGLE),
               new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_AROMATIC)));
            else if (order == _BOND_DOUBLE_OR_AROMATIC)
               bond.reset(QueryMolecule::Bond::oder(
               new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_DOUBLE),
               new QueryMolecule::Bond(QueryMolecule::BOND_ORDER, BOND_AROMATIC)));
            else if (order == _BOND_ANY)
               bond.reset(new QueryMolecule::Bond());
            else
               throw Error("unknown bond type: %d", order);

            _qmol->addBond(beg, end, bond.release());
         }

         while (true)
         {
            strscan.skipSpace();
            if (strscan.isEOF())
               break;

            QS_DEF(Array<char>, prop);

            strscan.readWord(prop, "=");
            strscan.skip(1);

            if (strcmp(prop.ptr(), "CFG") == 0)
            {
               int n = strscan.readInt1();

               if (n == 1)
                  _bmol->setBondDirection(i, BOND_UP);
               else if (n == 3)
                  _bmol->setBondDirection(i, BOND_DOWN);
               else if (n == 2)
               {
                  int bond_order = _bmol->getBondOrder(i);
                  if (bond_order == BOND_SINGLE)
                     _bmol->setBondDirection(i, BOND_EITHER);
                  else if (bond_order == BOND_DOUBLE)
                     _ignore_cistrans[i] = 1;
                  else
                     throw Error("unknown bond CFG=%d for thebond order %d", n, bond_order);
               }
               else
                  throw Error("unknown bond CFG=%d", n);
            }
            else if (strcmp(prop.ptr(), "STBOX") == 0)
            {
               if (_qmol == 0)
                  throw Error("stereo care box is allowed only for queries");

               if ((strscan.readInt1() != 0))
                  _stereo_care_bonds[i] = 1;
            }
            else if (strcmp(prop.ptr(), "TOPO") == 0)
            {
               if (_qmol == 0)
                  throw Error("bond topology setting is allowed only for queries");
                  
               int topo = strscan.readInt1();
               
               _qmol->resetBond(i, QueryMolecule::Bond::und(_qmol->releaseBond(i),
                  new QueryMolecule::Bond(QueryMolecule::BOND_TOPOLOGY,
                                          topo == 1 ? TOPOLOGY_RING : TOPOLOGY_CHAIN)));

            }
            else if (strcmp(prop.ptr(), "RXCTR") == 0)
               reacting_center = strscan.readInt1();
         }

         if (reaction_bond_reacting_center != 0)
            reaction_bond_reacting_center->at(i) = reacting_center;
      }

      _scanner.readLine(str, true);
      if (strncmp(str.ptr(), "M  V30 END BOND", 15) != 0)
         throw Error("Error reading BOND block footer");

      _scanner.readLine(str, true);
   }

   // Read collections and sgroups
   // There is no predefined order: sgroups may appear before collection
   bool collection_parsed = false, sgroups_parsed = false;
   while (strncmp(str.ptr(), "M  V30 END CTAB", 15) != 0)
   {
      if (strncmp(str.ptr(), "M  V30 BEGIN COLLECTION", 23) == 0)
      {
         if (collection_parsed)
            throw Error("COLLECTION block has already been parsed");
         _readCollectionBlock3000();
         collection_parsed = true;
      }
      else if (strncmp(str.ptr(), "M  V30 BEGIN SGROUP", 19) == 0)
      {
         if (sgroups_parsed)
            throw Error("SGROUP block has already been parsed");
         _readSGroupsBlock3000();
         sgroups_parsed = true;
      }
      else if (strncmp(str.ptr(), "M  V30 LINKNODE", 15) == 0)
         throw Error("link nodes are not supported yet (%s)", str.ptr());
      else
         throw Error("error reading CTAB block footer: %s", str.ptr());

      _scanner.readLine(str, true);
   }
}

void MolfileLoader::_readSGroupsBlock3000 ()
{
   QS_DEF(Array<char>, str);

   while (1)
   {
      _readMultiString(str);

      if (strncmp(str.ptr(), "END SGROUP", 10) == 0)
         break;
      if (STRCMP(str.ptr(), "M  V30 DEFAULT") == 0)
         continue;
      _readSGroup3000(str.ptr());
   }
}

void MolfileLoader::_readCollectionBlock3000 ()
{
   QS_DEF(Array<char>, str);

   while (1)
   {
      _readMultiString(str);

      if (strncmp(str.ptr(), "END COLLECTION", 14) == 0)
         break;

      BufferScanner strscan(str.ptr());
      char coll[14];

      strscan.readCharsFix(13, coll);
      coll[13] = 0;

      int stereo_type = 0;
      int stereo_group = 0;
      int n = 0;

      if (strcmp(coll, "MDLV30/STERAC") == 0)
         stereo_type = MoleculeStereocenters::ATOM_AND;
      else if (strcmp(coll, "MDLV30/STEREL") == 0)
         stereo_type = MoleculeStereocenters::ATOM_OR;
      else if (strcmp(coll, "MDLV30/STEABS") == 0)
         stereo_type = MoleculeStereocenters::ATOM_ABS;
      else if (strcmp(coll, "MDLV30/HILITE") == 0)
      {
         QS_DEF(Array<char>, what);

         strscan.skipSpace();
         strscan.readWord(what, " =");

         if (strcmp(what.ptr(), "ATOMS") == 0)
         {
            strscan.skip(2); // =(
            n = strscan.readInt1();
            while (n -- > 0)
               _bmol->highlightAtom(strscan.readInt1() - 1);
         }
         else if (strcmp(what.ptr(), "BONDS") == 0)
         {
            strscan.skip(2); // =(
            n = strscan.readInt1();
            while (n -- > 0)
               _bmol->highlightBond(strscan.readInt1() - 1);
         }
         else
            throw Error("unknown highlighted object: %s", what.ptr());
            
         continue;
      }
      else
         throw Error("unknown collection: %s", coll);

      if (stereo_type == MoleculeStereocenters::ATOM_OR ||
            stereo_type == MoleculeStereocenters::ATOM_AND)
         stereo_group = strscan.readInt1();
      else
         strscan.skip(1);

      strscan.skip(7); // ATOMS=(
      n = strscan.readInt1();
      while (n -- > 0)
      {
         int atom_idx = strscan.readInt1() - 1;
            
         _stereocenter_types[atom_idx] = stereo_type;
         _stereocenter_groups[atom_idx] = stereo_group;
      }
   }
}

void MolfileLoader::_preparePseudoAtomLabel (Array<char> &pseudo)
{
   // if the string is quoted, unquote it
   if (pseudo.size() > 2 && pseudo[0] == '\'' && pseudo[pseudo.size() - 2] == '\'')
   {
      pseudo.remove(pseudo.size() - 2);
      pseudo.remove(0);
   }

   if (pseudo.size() <= 1)
      throw Error("empty pseudo-atom");
}

void MolfileLoader::_readMultiString (Array<char> &str)
{
   QS_DEF(Array<char>, tmp);

   str.clear();
   tmp.clear_resize(7);

   while (1)
   {
      bool to_next = false;

      _scanner.read(7, tmp.ptr());
      if (strncmp(tmp.ptr(), "M  V30 ", 7) != 0)
         throw Error("error reading multi-string in CTAB v3000");

      _scanner.readLine(tmp, true);

      if (tmp[tmp.size() - 2] == '-')
      {
         tmp[tmp.size() - 2] = 0;
         tmp.pop();
         to_next = true;
      }
      str.appendString(tmp.ptr(), true);
      if (!to_next)
         break;
   }
}

void MolfileLoader::_readStringInQuotes (Scanner &scanner, Array<char> *str)
{
   char first = scanner.readChar();
   if (first == ' ')
      return;

   // Check if data is already present, then append to it using new line
   if (str && str->size() > 0)
   {
      if (str->top() == 0)
         str->pop();
      str->push('\n');
   }

   // If label is in quotes then read till the end quote
   bool in_quotes = (first == '"');
   if (!in_quotes && str)
      str->push(first);

   while (!scanner.isEOF())
   {
      char c = scanner.readChar();
      if (in_quotes)
      {
         // Break if other quote is reached
         if (c == '"')
            break;
      }
      else
      {
         // Break if space is reached
         if (isspace(c))
            break;
      }
      if (str)
         str->push(c);
   }
   if (str)
      str->push(0);
}

void MolfileLoader::_readRGroups3000 ()
{
   QS_DEF(Array<char>, str);

   MoleculeRGroups *rgroups = &_bmol->rgroups;

   while (!_scanner.isEOF())
   {
      _scanner.readLine(str, true);

      if (strncmp(str.ptr(), "M  V30 BEGIN RGROUP", 19) == 0)
      {
         _rgfile = true;

         int rg_idx;

         if (sscanf(str.ptr(), "M  V30 BEGIN RGROUP %d", &rg_idx) != 1)
            throw Error("can not read rgroup index");

         RGroup &rgroup = rgroups->getRGroup(rg_idx);

         _readMultiString(str);

         BufferScanner strscan(str.ptr());

         if (strncmp(str.ptr(), "RLOGIC", 6) != 0)
            throw Error("Error reading RGROUP block");

         strscan.skip(7);
         rgroup.if_then = strscan.readInt1();
         rgroup.rest_h = strscan.readInt1();

         if (!strscan.isEOF())
         {
            QS_DEF(Array<char>, occ);

            strscan.readLine(occ, true);
            _readRGroupOccurrenceRanges(occ.ptr(), rgroup.occurrence);
         }

         while (!_scanner.isEOF())
         {
            int pos = _scanner.tell();

            _scanner.readLine(str, true);
            if (strcmp(str.ptr(), "M  V30 BEGIN CTAB") == 0)
            {
               _scanner.seek(pos, SEEK_SET);
               AutoPtr<BaseMolecule> fragment(_bmol->neu());

               MolfileLoader loader(_scanner);
               loader._bmol = fragment.get();
               if (_bmol->isQueryMolecule())
               {
                  loader._qmol = &fragment.get()->asQueryMolecule();
                  loader._mol = 0;
               }
               else
               {
                  loader._qmol = 0;
                  loader._mol = &fragment.get()->asMolecule();
               }
               loader._readCtab3000();
               loader._postLoad();
               rgroup.fragments.add(fragment.release());
            }
            else if (strcmp(str.ptr(), "M  V30 END RGROUP") == 0)
               break;
            else
               throw Error("unexpected string in rgroup: %s", str.ptr());
         }

      }
      else if (strncmp(str.ptr(), "M  END", 6) == 0)
         break;
      else
         throw Error("unexpected string in rgroup: %s", str.ptr());
   }
}

void MolfileLoader::_readSGroup3000 (const char *str)
{
   BufferScanner scanner(str);
   QS_DEF(Array<char>, type);
   QS_DEF(Array<char>, entity);

   scanner.skipSpace();
   scanner.readInt();
   scanner.skipSpace();
   scanner.readWord(type, 0);
   type.push(0);
   scanner.skipSpace();
   scanner.readInt();
   scanner.skipSpace();

   BaseMolecule::SGroup *sgroup;
   BaseMolecule::DataSGroup *dsg = 0;
   BaseMolecule::Superatom *sup = 0;
   BaseMolecule::RepeatingUnit *sru = 0;

   if (strcmp(type.ptr(), "SUP") == 0)
   {
      sup = &_bmol->superatoms[_bmol->superatoms.add()];
      sgroup = sup;
   }
   else if (strcmp(type.ptr(), "DAT") == 0)
   {
      dsg = &_bmol->data_sgroups[_bmol->data_sgroups.add()];
      sgroup = dsg;
   }
   else if (strcmp(type.ptr(), "SRU") == 0)
   {
      sru = &_bmol->repeating_units[_bmol->repeating_units.add()];
      sgroup = sru;
   }
   else if (strcmp(type.ptr(), "MUL") == 0)
      sgroup = &_bmol->multiple_groups[_bmol->multiple_groups.add()];
   else if (strcmp(type.ptr(), "GEN") == 0)
      sgroup = &_bmol->generic_sgroups[_bmol->generic_sgroups.add()];
   else
      return; // unsupported kind of SGroup

   int n;

   while (!scanner.isEOF())
   {
      scanner.readWord(entity, "=");
      if (scanner.isEOF())
         return; // should not actually happen
      scanner.skip(1); // =
      entity.push(0);
      if (strcmp(entity.ptr(), "ATOMS") == 0)
      {
         scanner.skip(1); // (
         n = scanner.readInt1();
         while (n -- > 0)
         {
            sgroup->atoms.push(scanner.readInt() - 1);
            scanner.skipSpace();
         }
         scanner.skip(1); // )
      }
      else if (strcmp(entity.ptr(), "XBONDS") == 0)
      {
         scanner.skip(1); // (
         n = scanner.readInt1();
         while (n-- > 0)
         {
            int idx = scanner.readInt() - 1;
            if (sup != 0)
               sup->bond_idx = idx;
            scanner.skipSpace();
         }
         scanner.skip(1); // )
      }
      else if (strcmp(entity.ptr(), "PATOMS") == 0)
      {
         scanner.skip(1); // (
         n = scanner.readInt1();
         while (n -- > 0)
         {
            int idx = scanner.readInt() - 1;

            if (strcmp(type.ptr(), "MUL") == 0)
               ((BaseMolecule::MultipleGroup *)sgroup)->parent_atoms.push(idx);

            scanner.skipSpace();
         }
         scanner.skip(1); // )
      }
      else if (strcmp(entity.ptr(), "MULT") == 0)
      {
         int mult = scanner.readInt();
         if (strcmp(type.ptr(), "MUL") == 0)
            ((BaseMolecule::MultipleGroup *)sgroup)->multiplier = mult;
      }
      else if (strcmp(entity.ptr(), "BRKXYZ") == 0)
      {
         scanner.skip(1); // (
         n = scanner.readInt1();
         if (n != 9)
            throw Error("BRKXYZ number is %d (must be 9)", n);

         scanner.skipSpace();
         float x1 = scanner.readFloat(); scanner.skipSpace();
         float y1 = scanner.readFloat(); scanner.skipSpace();
         scanner.readFloat(); scanner.skipSpace(); // skip z
         float x2 = scanner.readFloat(); scanner.skipSpace();
         float y2 = scanner.readFloat(); scanner.skipSpace();
         scanner.readFloat(); scanner.skipSpace(); // skip z
         // skip 3-rd point
         scanner.readFloat(); scanner.skipSpace();
         scanner.readFloat(); scanner.skipSpace();
         scanner.readFloat(); scanner.skipSpace();
         Vec2f *brackets = sgroup->brackets.push();
         brackets[0].set(x1, y1);
         brackets[1].set(x2, y2);
         scanner.skip(1); // )
      }
      else if (strcmp(entity.ptr(), "CONNECT") == 0)
      {
         char c1 = scanner.readChar();
         char c2 = scanner.readChar();

         if (strcmp(type.ptr(), "SRU") == 0)
         {
            BaseMolecule::RepeatingUnit &ru = *(BaseMolecule::RepeatingUnit *)sgroup;
            if (c1 == 'H' && c2 == 'T')
               ru.connectivity = BaseMolecule::RepeatingUnit::HEAD_TO_TAIL;
            else if (c1 == 'H' && c2 == 'H')
               ru.connectivity = BaseMolecule::RepeatingUnit::HEAD_TO_HEAD;
         }
      }
      else if (strcmp(entity.ptr(), "FIELDNAME") == 0)
      {
         _readStringInQuotes(scanner, dsg ? &dsg->description : NULL);
      }
      else if (strcmp(entity.ptr(), "FIELDDISP") == 0)
      {
         QS_DEF(Array<char>, substr);
         _readStringInQuotes(scanner, &substr);
         if (dsg != 0)
         {
            BufferScanner subscan(substr);
            _readSGroupDisplay(subscan, *dsg);
         }
      }
      else if (strcmp(entity.ptr(), "FIELDDATA") == 0)
      {
         _readStringInQuotes(scanner, &dsg->data);
      }
      else if (strcmp(entity.ptr(), "LABEL") == 0)
      {
         while (!scanner.isEOF())
         {
            char c = scanner.readChar();
            if (c == ' ')
               break;
            if (sup != 0)
               sup->subscript.push(c);
            if (sru != 0)
               sru->subscript.push(c);
         }
         if (sup != 0)
            sup->subscript.push(0);
         if (sru != 0)
            sru->subscript.push(0);
      }
      else 
      {
         if (scanner.lookNext() == '(')
         {
            scanner.skip(1);
            n = scanner.readInt1();
            while (n -- > 0)
            {
               scanner.readFloat();
               scanner.skipSpace();
            }
            scanner.skip(1); // )
         }
         else
         {
            // Unknown property that can have value in quotes: PROP="     "
            _readStringInQuotes(scanner, NULL);
         }
      }
      scanner.skipSpace();
   }
}


void MolfileLoader::_readSGroupDisplay (Scanner &scanner, BaseMolecule::DataSGroup &dsg)
{
   dsg.display_pos.x = scanner.readFloatFix(10);
   dsg.display_pos.y = scanner.readFloatFix(10);
   scanner.skip(4);
   if (scanner.readChar() == 'A') // means "attached"
      dsg.detached = false;
   else
      dsg.detached = true;
   if (scanner.readChar() == 'R')
      dsg.relative = true;
   if (scanner.readChar() == 'U')
      dsg.display_units = true;

   int cur = scanner.tell();
   scanner.seek(0, SEEK_END);
   int end = scanner.tell();
   scanner.seek(cur, SEEK_SET);

   if (end - cur + 1 > 16)
   {
      scanner.skip(16);
      int c = scanner.readChar();
      if (c >= '1' && c <= '9')
         dsg.dasp_pos = c - '0';
   }
}


void MolfileLoader::_init ()
{
   _hcount.clear();
   _atom_types.clear();
   _sgroup_types.clear();
   _sgroup_mapping.clear();
   _stereo_care_atoms.clear_resize(_atoms_num);
   _stereo_care_atoms.zerofill();
   _stereo_care_bonds.clear_resize(_bonds_num);
   _stereo_care_bonds.zerofill();
   _stereocenter_types.clear_resize(_atoms_num);
   _stereocenter_types.zerofill();
   _stereocenter_groups.clear_resize(_atoms_num);
   _stereocenter_groups.zerofill();
   _sensible_bond_directions.clear_resize(_bonds_num);
   _sensible_bond_directions.zerofill();
   _ignore_cistrans.clear_resize(_bonds_num);
   _ignore_cistrans.zerofill();

   _stereo_care_bonds.clear_resize(_bonds_num);
   _stereo_care_bonds.zerofill();

   if (reaction_atom_mapping != 0)
      reaction_atom_mapping->clear_resize(_atoms_num);
   if (reaction_atom_inversion != 0)
      reaction_atom_inversion->clear_resize(_atoms_num);
   if (reaction_atom_exact_change != 0)
      reaction_atom_exact_change->clear_resize(_atoms_num);
   if (reaction_bond_reacting_center != 0)
      reaction_bond_reacting_center->clear_resize(_bonds_num);
}
