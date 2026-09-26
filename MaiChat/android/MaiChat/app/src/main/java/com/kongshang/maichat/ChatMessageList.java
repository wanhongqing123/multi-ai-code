package com.kongshang.maichat;

import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.ListView;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;

/** Recycles rows, preserves a visible message anchor, and only scrolls on explicit intent. */
final class ChatMessageList extends ListView {
    interface Renderer { View messageView(RemoteIMMessage message); }
    private final Renderer renderer;
    private final Rows adapter = new Rows();
    private List<RemoteIMMessage> messages = new ArrayList<>();
    private RemoteIMActivitySignal activity;
    private final Map<String, Long> stableIds = new HashMap<>();
    private long nextId;
    private int bindingRevision;
    private enum Position { NONE, LATEST, MESSAGE_BOTTOM, SEARCH, RESTORE }
    private Position position = Position.NONE;
    private String targetId;
    private int targetOffset;
    private long intentGeneration;
    private boolean keyboardPin;
    private long keyboardIntent;
    private String keyboardAnchor;
    private int bottomPadding = -1;

    ChatMessageList(Context context, Renderer renderer) {
        super(context);
        this.renderer = renderer;
        setDivider(null); setDividerHeight(0); setCacheColorHint(android.graphics.Color.TRANSPARENT);
        setSelector(new android.graphics.drawable.ColorDrawable(android.graphics.Color.TRANSPARENT));
        setItemsCanFocus(true);
        setStackFromBottom(false); setTranscriptMode(TRANSCRIPT_MODE_DISABLED);
        setAdapter(adapter);
    }
    void requestLatestAfterUpdate() { position = Position.LATEST; targetId = null; intentGeneration++; }
    void showLatest() { requestLatestAfterUpdate(); requestLayout(); }
    void beginKeyboardReveal() {
        showLatest(); keyboardPin = true; keyboardIntent = intentGeneration; keyboardAnchor = null;
    }
    private boolean pinsKeyboard() { return keyboardPin && keyboardIntent == intentGeneration; }
    void endKeyboardReveal() {
        if (pinsKeyboard() && keyboardAnchor != null && position == Position.NONE) {
            position = Position.MESSAGE_BOTTOM; targetId = keyboardAnchor; requestLayout();
        }
        keyboardPin = false; keyboardAnchor = null;
    }
    void requestMessageBottomAfterUpdate(String id) {
        position = Position.MESSAGE_BOTTOM; targetId = id; intentGeneration++;
    }
    void showMessage(String id) {
        position = Position.SEARCH; targetId = id; intentGeneration++; requestLayout();
    }
    long cancelPositionIntent() { position = Position.NONE; targetId = null; return ++intentGeneration; }
    boolean acceptsIntent(long generation) { return intentGeneration == generation; }
    String firstVisibleMessageId() {
        if (getChildCount() == 0) return null;
        Object tag = getChildAt(0).getTag();
        if (tag instanceof Binding && ((Binding) tag).message != null) return ((Binding) tag).message.id();
        return null;
    }
    int firstVisibleOffset() { return getChildCount() > 0 ? getChildAt(0).getTop() - getPaddingTop() : 0; }
    void restoreMessage(String id, int offset) {
        position = Position.RESTORE; targetId = id; targetOffset = offset; requestLayout();
    }
    private int targetPosition() {
        if (position == Position.LATEST) return adapter.getCount() - 1;
        for (int index = 0; index < adapter.getCount(); index++) if (key(index).equals(targetId)) return index;
        return -1;
    }
    @Override protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
        // Let ListView consume dataset synchronization first. Applying an intent
        // before this point can be overwritten by its saved selection/row ID.
        super.onLayout(changed, left, top, right, bottom);
        if (position == Position.NONE && pinsKeyboard() && keyboardAnchor != null) {
            position = Position.MESSAGE_BOTTOM; targetId = keyboardAnchor;
        }
        if (position == Position.NONE) return;
        int target = targetPosition();
        if (target < 0) return;
        if (pinsKeyboard() && keyboardAnchor == null) keyboardAnchor = key(target);
        Position requested = position;
        int requestedOffset = targetOffset;
        position = Position.NONE; targetId = null;
        boolean allRowsFit = getFirstVisiblePosition() == 0
            && getLastVisiblePosition() == adapter.getCount() - 1;
        if (allRowsFit && (requested == Position.LATEST || requested == Position.MESSAGE_BOTTOM)) {
            setSelectionFromTop(0, 0);
            layoutChildren();
            return;
        }
        int offset = requested == Position.RESTORE ? requestedOffset : requested == Position.SEARCH ? getHeight() / 3 : 0;
        setSelectionFromTop(target, offset);
        layoutChildren();
        if (requested == Position.LATEST || requested == Position.MESSAGE_BOTTOM) {
            View row = getChildAt(target - getFirstVisiblePosition());
            if (row != null) {
                int bottomReserve = getPaddingBottom();
                if (requested == Position.MESSAGE_BOTTOM && target == messages.size() - 1 && activity != null) {
                    bottomReserve += Math.round(48 * getResources().getDisplayMetrics().density);
                }
                int y = getHeight() - bottomReserve - getPaddingTop() - row.getHeight();
                setSelectionFromTop(target, y);
                layoutChildren();
            }
        }
        // Both layout passes occur before drawing; there is no delayed second scroll.
    }
    void update(List<RemoteIMMessage> values, RemoteIMActivitySignal nextActivity, int revision) {
        String anchor = firstVisibleMessageId();
        int offset = firstVisibleOffset();
        boolean sameActivity = activity == null ? nextActivity == null : nextActivity != null
            && activity.activityId.equals(nextActivity.activityId) && activity.kind == nextActivity.kind
            && activity.startedAtMs == nextActivity.startedAtMs
            && activity.taskStartedAtMs == nextActivity.taskStartedAtMs;
        boolean changed = !messages.equals(values) || !sameActivity || bindingRevision != revision;
        if (changed) {
            // Reserve the next reply slot while idle. Showing a typing row then
            // trades padding for a row instead of shifting history or scrolling.
            if (bottomPadding < 0) bottomPadding = getPaddingBottom();
            int reserve = nextActivity == null ? Math.round(48 * getResources().getDisplayMetrics().density) : 0;
            setPadding(getPaddingLeft(), getPaddingTop(), getPaddingRight(), bottomPadding + reserve);
            messages = new ArrayList<>();
            for (RemoteIMMessage message : values) messages.add(message.snapshot());
            activity = nextActivity; bindingRevision = revision;
            Map<String, Long> ids = new HashMap<>();
            for (int i = 0; i < adapter.getCount(); i++) {
                String key = key(i);
                ids.put(key, stableIds.containsKey(key) ? stableIds.get(key) : ++nextId);
            }
            stableIds.clear(); stableIds.putAll(ids);
            adapter.notifyDataSetChanged();
        }
        if (pinsKeyboard() && keyboardAnchor != null && position == Position.NONE) {
            position = Position.MESSAGE_BOTTOM; targetId = keyboardAnchor; requestLayout();
        } else if ((position == Position.NONE || position == Position.RESTORE) && changed && anchor != null) {
            restoreMessage(anchor, offset);
        } else if (position != Position.NONE) requestLayout();
    }
    private String key(int position) {
        return position < messages.size() ? messages.get(position).id() : "activity:" + activity.activityId;
    }
    private static final class Binding {
        RemoteIMMessage message;
        RemoteIMActivitySignal.Kind activityKind;
        String activityId;
        long activityStartedAtMs, taskStartedAtMs;
        int revision;
    }
    private final class Rows extends BaseAdapter {
        @Override public int getCount() { return messages.size() + (activity == null ? 0 : 1); }
        @Override public Object getItem(int position) { return position < messages.size() ? messages.get(position) : activity; }
        @Override public long getItemId(int position) { return stableIds.getOrDefault(key(position), -1L); }
        @Override public boolean hasStableIds() { return true; }
        @Override public int getViewTypeCount() { return 2; }
        @Override public int getItemViewType(int position) { return position < messages.size() ? 0 : 1; }
        @Override public boolean isEnabled(int position) { return true; }
        @Override public View getView(int position, View convertView, ViewGroup parent) {
            Binding previous = convertView != null && convertView.getTag() instanceof Binding ? (Binding) convertView.getTag() : null;
            Binding next = new Binding(); next.revision = bindingRevision;
            if (position < messages.size()) {
                next.message = messages.get(position);
                if (previous != null && Objects.equals(previous.message, next.message) && previous.revision == next.revision) return convertView;
                convertView = renderer.messageView(next.message);
            } else {
                next.activityKind = activity.kind; next.activityId = activity.activityId;
                next.activityStartedAtMs = activity.startedAtMs;
                next.taskStartedAtMs = activity.taskStartedAtMs;
                if (previous != null && previous.activityKind == next.activityKind
                    && Objects.equals(previous.activityId, next.activityId)
                    && previous.activityStartedAtMs == next.activityStartedAtMs
                    && previous.taskStartedAtMs == next.taskStartedAtMs) return convertView;
                convertView = new ActivityBubbleView(getContext(), activity);
            }
            convertView.setTag(next);
            return convertView;
        }
    }
}
