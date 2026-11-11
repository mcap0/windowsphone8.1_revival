/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * A page that takes the back key first (leaving full screen, closing an overlay).
 */

namespace Perle81.App
{
    public interface IBackHandler
    {
        /// <returns>true when the page used the back key</returns>
        bool HandleBack();
    }
}
