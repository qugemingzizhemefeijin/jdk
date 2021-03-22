/*
 * Copyright (c) 1998, 2021, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

package jdk.javadoc.internal.doclets.toolkit.util;

import java.util.*;

import javax.lang.model.element.Element;

import jdk.javadoc.internal.doclets.toolkit.BaseConfiguration;

/**
 * Build list of all the deprecated packages, classes, constructors, fields and methods.
 *
 *  <p><b>This is NOT part of any supported API.
 *  If you write code that depends on this, you do so at your own risk.
 *  This code and its internal interfaces are subject to change or
 *  deletion without notice.</b>
 */
public class DeprecatedAPIListBuilder extends SummaryAPIListBuilder {

    private SortedSet<Element> forRemoval;
    public final List<PerReleaseBuilder> releases = new ArrayList<>();

    /**
     * Constructor.
     *
     * @param configuration the current configuration of the doclet
     */
    public DeprecatedAPIListBuilder(BaseConfiguration configuration, List<String> summaries) {
        super(configuration, configuration.utils::isDeprecated);
        buildSummaryAPIInfo();
        summaries.forEach(release -> {
            PerReleaseBuilder builder = new PerReleaseBuilder(configuration, release);
            if (!builder.isEmpty()) {
                releases.add(builder);
            }
        });
    }

    public SortedSet<Element> getForRemoval() {
        if (forRemoval == null) {
            forRemoval = createSummarySet();
        }
        return forRemoval;
    }

    @Override
    protected void handleElement(Element e) {
        if (utils.isDeprecatedForRemoval(e)) {
            getForRemoval().add(e);
        }
    }

    public static class PerReleaseBuilder extends SummaryAPIListBuilder {

        public final String release;
        private SortedSet<Element> forRemoval;


        /**
         * Constructor.
         *
         * @param configuration the current configuration of the doclet
         * @param release single release id to document new APIs for
         */
        public PerReleaseBuilder(BaseConfiguration configuration, String release) {
            super(configuration, e -> isDeprecatedSince(e, release));
            this.release = release;
            buildSummaryAPIInfo();
        }

        private static boolean isDeprecatedSince(Element e, String release) {
            Deprecated[] depr = e.getAnnotationsByType(Deprecated.class);
            if (depr != null && depr.length > 0) {
                return release.equals(depr[0].since());
            }
            return false;
        }

        public SortedSet<Element> getForRemoval() {
            if (forRemoval == null) {
                forRemoval = createSummarySet();
            }
            return forRemoval;
        }

        @Override
        protected void handleElement(Element e) {
            if (isDeprecatedSince(e, release) && utils.isDeprecatedForRemoval(e)) {
                getForRemoval().add(e);
            }
        }

    }

}
