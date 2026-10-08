/* =====================================================
   SHARED
   =====================================================
   Small GUI helpers shared by multiple touchscreen pages.
*/

// Keeps form controls usable on the touchscreen when the on-screen keyboard or a
// constrained modal would otherwise leave the selected field outside the comfortable
// visible area. The container and selector are supplied by whichever page uses it.
function enableTouchInputScrolling(container, fields) {
    if (!container) {
        return;
    }

    // The same helper can serve multiple forms by attaching the behaviour to every
    // control matched by the selector supplied by the calling page.
    document.querySelectorAll(fields).forEach(field => {
        field.addEventListener("pointerdown", event => {
            const containerRect =
                container.getBoundingClientRect();

            const fieldRect =
                field.getBoundingClientRect();

            // Define a small vertical safe zone inside the scroll container. A selected
            // field outside this zone is moved before focus is given to the control.
            const safeTop =
                containerRect.top + 80;

            const safeBottom =
                containerRect.top + 180;

            let scrollAmount = 0;

            if (fieldRect.top < safeTop) {
                scrollAmount =
                    fieldRect.top - safeTop;
            } else if (
                fieldRect.bottom > safeBottom
            ) {
                scrollAmount =
                    fieldRect.bottom - safeBottom;
            }

            if (scrollAmount === 0) {
                return;
            }

            event.preventDefault();

            // When scrolling downward near the end of the container, temporary bottom
            // padding creates enough scrollable space to bring the field fully into view.
            if (scrollAmount > 0) {
                const currentMaximum =
                    container.scrollHeight -
                    container.clientHeight;

                const requestedPosition =
                    container.scrollTop +
                    scrollAmount;

                if (
                    requestedPosition >
                    currentMaximum
                ) {
                    const extraSpace =
                        requestedPosition -
                        currentMaximum +
                        40;

                    container.style.paddingBottom =
                        extraSpace + "px";
                }
            }

            // Move first, then focus shortly afterwards. This avoids the browser's normal
            // focus behaviour fighting with the custom touchscreen scroll position.
            container.scrollBy({
                top: scrollAmount,
                behavior: "instant"
            });

            setTimeout(() => {
                field.focus();

                // Selecting existing input text makes touchscreen editing quicker; other
                // control types are focused normally without attempting text selection.
                if (
                    field.tagName === "INPUT"
                ) {
                    field.select();
                }
            }, 50);
        });
    });
}
