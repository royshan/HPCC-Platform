/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */


namesRecord :=
            RECORD
string20        surname;
string10        forename;
integer2        age := 25;
            END;

namesTable := dataset([
        {'Smithe','Pru',10},
        {'Hawthorn','Gavin',31},
        {'Hawthorn','Mia',30},
        {'X','Z'}], namesRecord);

sort1 := sort(namesTable, forename, LOCAL, unstable('Quick'));
sort2 := sort(namesTable, forename, SKEW(0.5));
sort3 := sort(namesTable, forename, THRESHOLD(1000000));
sort4 := sort(namesTable, forename, SKEW(0.1), THRESHOLD(99999));
output(sort1+sort2+sort3+sort4,,'out.d00');

sort5 := sort(group(namesTable, surname), forename, many);
sort6 := sort(namesTable, forename, skew(1.1,3.4));
output(sort5+sort6);
