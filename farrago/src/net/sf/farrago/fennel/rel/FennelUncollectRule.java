/*
// $Id$
// Farrago is an extensible data management system.
// Copyright (C) 2005-2009 The Eigenbase Project
// Copyright (C) 2002-2009 SQLstream, Inc.
// Copyright (C) 2009-2009 LucidEra, Inc.
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
package net.sf.farrago.fennel.rel;

import net.sf.farrago.query.*;

import org.eigenbase.rel.*;
import org.eigenbase.relopt.*;


/**
 * FennelUncollectRule is a rule to implement a call with the {@link
 * org.eigenbase.sql.fun.SqlStdOperatorTable#unnestOperator}
 *
 * @author Wael Chatila
 * @version $Id$
 * @since Dec 12, 2004
 */
public class FennelUncollectRule
    extends RelOptRule
{
    public static final FennelUncollectRule instance =
        new FennelUncollectRule();

    //~ Constructors -----------------------------------------------------------

    /**
     * @deprecated use {@link #instance} instead
     */
    public FennelUncollectRule()
    {
        super(
            new RelOptRuleOperand(
                UncollectRel.class,
                ANY));
    }

    //~ Methods ----------------------------------------------------------------

    // implement RelOptRule
    public CallingConvention getOutConvention()
    {
        return FennelRel.FENNEL_EXEC_CONVENTION;
    }

    public void onMatch(RelOptRuleCall call)
    {
        UncollectRel uncollectRel = (UncollectRel) call.rels[0];
        RelNode relInput = uncollectRel.getChild();
        RelNode fennelInput =
            mergeTraitsAndConvert(
                uncollectRel.getTraits(),
                FennelRel.FENNEL_EXEC_CONVENTION,
                relInput);
        if (fennelInput == null) {
            return;
        }

        FennelPullUncollectRel fennelUncollectRel =
            new FennelPullUncollectRel(
                uncollectRel.getCluster(),
                fennelInput);
        call.transformTo(fennelUncollectRel);
    }
}

// End FennelUncollectRule.java