/*
// $Id$
// Farrago is an extensible data management system.
// Copyright (C) 2007-2007 The Eigenbase Project
// Copyright (C) 2007-2007 Disruptive Tech
// Copyright (C) 2007-2007 LucidEra, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version approved by The Eigenbase Project.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

package net.sf.farrago.namespace;

import net.sf.farrago.fem.med.*;
import net.sf.farrago.fennel.*;

/**
 * FarragoMedLocalIndexStats represents index statistics collected via
 * {@link FarragoMedLocalDataServer#computeIndexStats(FemLocalIndex, long, boolean, FennelTxnContext)}.
 *
 * @author Stephan Zuercher
 */
public class FarragoMedLocalIndexStats
{
    private final long pageCount;
    private final long uniqueKeyCount;
    
    public FarragoMedLocalIndexStats(long pageCount, long uniqueKeyCount)
    {
        this.pageCount = pageCount;
        this.uniqueKeyCount = uniqueKeyCount;
    }
    
    public long getPageCount()
    {
        return pageCount;
    }
    
    public long getUniqueKeyCount()
    {
        return uniqueKeyCount;
    }
}